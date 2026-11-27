// SPDX-License-Identifier: LGPL-2.1-or-later
#include "journal.h"
#include "newesttree.h"

#include <QDataStream>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace
{
constexpr quint32 s_magic = 0x4e474a31; // "NGJ1"
constexpr qint64 s_keepMs = 14LL * 24 * 3600 * 1000;
constexpr int s_keepCount = 20000;
// more than this many files by one process, each within the window of the one before: a bulk change
constexpr int s_bulkFiles = 40;
constexpr qint64 s_bulkWindowMs = 10 * 1000;

QString commonFolder(const QString &a, const QString &b)
{
    int slash = 0;
    const int n = qMin(a.size(), b.size());
    int i = 0;
    for (; i < n && a.at(i) == b.at(i); ++i) {
        if (a.at(i) == QLatin1Char('/')) {
            slash = i;
        }
    }
    if (i == n && (a.size() == b.size() || (a.size() > n ? a.at(n) : b.at(n)) == QLatin1Char('/'))) {
        return a.left(n); // one is the other, or lies inside it
    }
    return a.left(slash);
}

// the first argument a process was started with, without its folder (readable by anyone; the executable's own
// path is not, without a capability this service does not keep)
QString programOf(int pid)
{
    QFile file(QStringLiteral("/proc/%1/cmdline").arg(pid));
    QByteArray first;
    if (file.open(QIODevice::ReadOnly)) {
        first = file.read(4096);
        first.truncate(qMax<qsizetype>(0, first.indexOf('\0')));
    }
    if (first.isEmpty()) {
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (comm.open(QIODevice::ReadOnly)) {
            first = comm.read(64).trimmed();
        }
    }
    // ("/usr/lib/firefox/firefox -contentproc ..." written as one argument by some programs)
    const QString text = QFile::decodeName(first).section(QLatin1Char(' '), 0, 0);
    return text.section(QLatin1Char('/'), -1).left(64);
}

// the desktop application a process belongs to, from the name of its systemd scope or service:
//   app-org.kde.kate-1a2b3c.scope, app-org.kde.kate@1a2b3c.service, app-flatpak-org.gimp.GIMP-123.scope,
//   snap.firefox.firefox-<uuid>.scope
QString appOf(int pid)
{
    QFile file(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString unit = QString::fromUtf8(file.read(4096)).trimmed().section(QLatin1Char('/'), -1);
    static const QRegularExpression app(QStringLiteral("^app-(?:flatpak-|gnome-)?(.+?)(?:-[0-9a-f]+|@[^.]*)?\\.(?:scope|service)$"));
    static const QRegularExpression snap(QStringLiteral("^snap\\.([^.]+)\\.[^.]+?(?:-[0-9a-f-]{36})?\\.scope$"));
    auto match = app.match(unit);
    if (match.hasMatch()) {
        // (systemd writes what it cannot have in a unit name as \xNN)
        QString id = match.captured(1);
        static const QRegularExpression escaped(QStringLiteral("\\\\x([0-9a-fA-F]{2})"));
        for (auto hex = escaped.match(id); hex.hasMatch(); hex = escaped.match(id)) {
            id.replace(hex.capturedStart(), hex.capturedLength(), QChar(hex.captured(1).toInt(nullptr, 16)));
        }
        return id.left(128);
    }
    match = snap.match(unit);
    return match.hasMatch() ? match.captured(1).left(128) : QString();
}
}

EventJournal::EventJournal(const NewestTree *tree)
    : m_tree(tree)
{
}

bool EventJournal::isNoise(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('~'))) {
        return true; // hidden, or a backup copy
    }
    if (name.startsWith(QLatin1Char('#')) && name.endsWith(QLatin1Char('#'))) {
        return true; // Emacs
    }
    static const QRegularExpression temporary(QStringLiteral("\\.(?:tmp|temp|part|partial|crdownload|download|swp|swx|lock|lck|bak|kate-swp)$|" // by extension
                                                             "\\.(?:tmp|temp)[.-]?[0-9A-Za-z]{2,}$|" // name.tmp123, name.tmp.Ab12Cd
                                                             "-(?:journal|wal|shm)$|" // SQLite
                                                             "^~\\$|^lu[0-9a-z]+\\.tmp$|^sed[A-Za-z0-9]{6}$"), // Office, LibreOffice, sed -i
                                              QRegularExpression::CaseInsensitiveOption);
    if (temporary.match(name).hasMatch()) {
        return true;
    }
    // name.Xy12Zw: six letters and digits of mixed case after the last dot, as mkstemp() and QSaveFile make them
    static const QRegularExpression random(QStringLiteral("\\.(?=[A-Za-z0-9]{6}$)(?=.*[A-Z])(?=.*[a-z])[A-Za-z0-9]{6}$"));
    return random.match(name).hasMatch();
}

bool EventJournal::wanted(const QString &folder, const QString &name) const
{
    return !isNoise(name) && m_tree->shown(folder);
}

EventJournal::Who EventJournal::who(int pid, qint64 when)
{
    if (pid <= 0) {
        return {};
    }
    // (asked again after a while: a pid is given to another process sooner or later)
    auto it = m_who.find(pid);
    if (it == m_who.end() || when - it->asked > 30 * 1000) {
        if (m_who.size() > 4096) {
            m_who.clear();
        }
        it = m_who.insert(pid, {programOf(pid), appOf(pid), when});
    }
    return *it;
}

void EventJournal::put(const QString &path, quint32 kinds, int pid, qint64 when)
{
    const Who by = who(pid, when);

    // many files by one process in a short time: one entry for the folder they share, not one each
    if (pid > 0 && !(kinds & FileEvent::Folder)) {
        if (m_bursts.size() > 1024) {
            m_bursts.clear();
        }
        Burst &burst = m_bursts[pid];
        if (when - burst.last > s_bulkWindowMs) {
            burst = Burst{};
            burst.started = when;
        }
        burst.last = when;
        const QString folder = path.left(path.lastIndexOf(QLatin1Char('/')));
        if (!burst.bulk) {
            if (!burst.paths.contains(path)) {
                burst.paths.append(path);
            }
            if (burst.paths.size() > s_bulkFiles) {
                burst.bulk = true;
                burst.common = folder;
                for (const QString &one : std::as_const(burst.paths)) {
                    m_entries.remove(one);
                    burst.common = commonFolder(burst.common, one.left(one.lastIndexOf(QLatin1Char('/'))));
                }
                for (const QString &one : std::as_const(burst.paths)) {
                    burst.counted.insert(qHash(one));
                }
                burst.paths.clear();
            }
        } else {
            if (burst.counted.size() > 200000) {
                burst.counted.clear();
            }
            burst.counted.insert(qHash(path));
            const QString common = commonFolder(burst.common, folder);
            if (common != burst.common) {
                m_entries.remove(burst.common);
                burst.common = common;
            }
        }
        if (burst.bulk) {
            if (m_tree->shown(burst.common)) {
                burst.count = qMax<quint32>(burst.count, burst.counted.size());
                if (!m_entries.contains(burst.common)) {
                    // the folders the same burst made inside it are part of the one entry
                    const QString prefix = burst.common + QLatin1Char('/');
                    const qint64 started = burst.started;
                    m_entries.removeIf([&](const std::pair<const QString &, Entry &> &entry) {
                        return entry.second.when >= started && (entry.second.kinds & FileEvent::Folder) && entry.first.startsWith(prefix);
                    });
                }
                Entry &entry = m_entries[burst.common];
                entry.when = when;
                entry.kinds = FileEvent::Folder | FileEvent::Bulk | FileEvent::Modified;
                entry.count = burst.count;
                entry.program = by.program;
                entry.app = by.app;
                m_dirty = true;
            }
            return;
        }
    }

    if ((kinds & FileEvent::Folder) && pid > 0) {
        const auto burst = m_bursts.constFind(pid);
        if (burst != m_bursts.cend() && burst->bulk && when - burst->last <= s_bulkWindowMs && path.startsWith(burst->common + QLatin1Char('/'))) {
            return; // a folder made by a bulk change inside the folder that stands for it
        }
    }
    Entry &entry = m_entries[path];
    // what is created and then written to within moments is still "created" (a copy, a download, a new document)
    const bool sameBurst = entry.when > 0 && when - entry.when < 60 * 1000;
    entry.kinds = (sameBurst ? (entry.kinds & (FileEvent::Created | FileEvent::Moved)) : 0) | kinds;
    entry.when = when;
    entry.count = 0;
    // (within those moments the one who made it stays: a program that touches it right after did not make it)
    const bool madeBefore = sameBurst && (entry.kinds & FileEvent::Created) && !(kinds & FileEvent::Created) && !entry.program.isEmpty();
    if (!madeBefore && (!by.program.isEmpty() || !sameBurst)) {
        entry.program = by.program;
        entry.app = by.app;
    }
    m_dirty = true;
}

void EventJournal::record(const QString &folder, const QString &name, quint32 kinds, int pid, qint64 when)
{
    if (wanted(folder, name)) {
        put(folder + QLatin1Char('/') + name, kinds, pid, when);
    }
}

void EventJournal::remove(const QString &folder, const QString &name, bool isFolder)
{
    const QString path = folder + QLatin1Char('/') + name;
    m_dirty |= m_entries.remove(path) > 0;
    if (isFolder) {
        const QString prefix = path + QLatin1Char('/');
        m_dirty |= m_entries.removeIf([&prefix](const std::pair<const QString &, Entry &> &entry) {
            return entry.first.startsWith(prefix);
        }) > 0;
    }
}

void EventJournal::temporaryMade(const QString &folder, const QString &name, qint64 when)
{
    if (!m_tree->shown(folder)) {
        return;
    }
    // the name it stands for: ".report.odt.Ab12Cd" and "report.odt.tmp" are "report.odt", "paper.pdf.part" is "paper.pdf"
    static const QRegularExpression decoration(QStringLiteral("^\\.?(.+\\.[^.]+)\\.(?:[A-Za-z0-9]{6}|tmp|temp|part|partial|crdownload|download)$"),
                                               QRegularExpression::CaseInsensitiveOption);
    const auto match = decoration.match(name);
    if (!match.hasMatch()) {
        return;
    }
    if (m_temps.size() > 2000) {
        m_temps.clear();
    }
    const QString becomes = match.captured(1);
    m_temps.insert(folder + QLatin1Char('/') + name, {when, becomes, !QFile::exists(folder + QLatin1Char('/') + becomes)});
}

void EventJournal::rename(const QString &oldFolder, const QString &oldName, const QString &newFolder, const QString &newName, bool isFolder, int pid, qint64 when)
{
    const QString oldPath = oldFolder + QLatin1Char('/') + oldName;
    const QString newPath = newFolder + QLatin1Char('/') + newName;
    const Entry old = m_entries.take(oldPath);
    const Temp temp = m_temps.take(oldPath);
    const bool kept = wanted(newFolder, newName);
    if (isFolder) {
        // what was inside is under the new name now (or out of sight: then it is dropped)
        const QString prefix = oldPath + QLatin1Char('/');
        QHash<QString, Entry> moved;
        m_entries.removeIf([&](const std::pair<const QString &, Entry &> &entry) {
            if (!entry.first.startsWith(prefix)) {
                return false;
            }
            if (kept) {
                moved.insert(newPath + entry.first.mid(oldPath.size()), entry.second);
            }
            return true;
        });
        m_dirty |= !moved.isEmpty();
        m_entries.insert(moved);
    }
    if (!kept) {
        return;
    }
    quint32 kinds = isFolder ? quint32(FileEvent::Folder) : 0;
    if (!isFolder && oldFolder == newFolder && isNoise(oldName)) {
        // written beside under a temporary name, then renamed over: that is how many programs save, and how a
        // browser finishes a download. New when nothing had the name before: the temporary file was made while
        // the name was free, or the name was only reserved (made and never written to)
        kinds |= FileEvent::Modified;
        const auto reserved = m_entries.constFind(newPath);
        if ((temp.fresh && temp.becomes == newName) || (reserved != m_entries.cend() && reserved->kinds == FileEvent::Created)) {
            kinds |= FileEvent::Created;
            m_entries.remove(newPath); // (put() keeps "created" only for a while: here it is known)
        }
    } else {
        kinds |= FileEvent::Moved;
        if (old.when > 0 && when - old.when < 60 * 1000) {
            kinds |= old.kinds & FileEvent::Created; // made a moment ago under another name
        }
    }
    put(newPath, kinds, pid, when);
}

QList<FileEvent> EventJournal::events(uint uid, qint64 since, int limit) const
{
    QList<FileEvent> list;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->when < since) {
            continue;
        }
        const QString path = m_tree->named(uid, it.key());
        if (!path.isEmpty()) {
            list.append({path, it->when, it->kinds, it->program, it->app, it->count});
        }
    }
    std::sort(list.begin(), list.end(), [](const FileEvent &a, const FileEvent &b) {
        return a.when > b.when;
    });
    if (list.size() > limit) {
        list.resize(limit);
    }
    return list;
}

int EventJournal::count(uint uid) const
{
    int n = 0;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        n += m_tree->named(uid, it.key()).isEmpty() ? 0 : 1;
    }
    return n;
}

void EventJournal::expire(qint64 now)
{
    const auto before = m_entries.size();
    m_entries.removeIf([now](const std::pair<const QString &, Entry &> &entry) {
        return now - entry.second.when > s_keepMs;
    });
    if (m_entries.size() > s_keepCount) {
        QList<qint64> times;
        times.reserve(m_entries.size());
        for (const Entry &entry : std::as_const(m_entries)) {
            times.append(entry.when);
        }
        std::nth_element(times.begin(), times.begin() + (times.size() - s_keepCount), times.end());
        const qint64 oldestKept = times.at(times.size() - s_keepCount);
        m_entries.removeIf([oldestKept](const std::pair<const QString &, Entry &> &entry) {
            return entry.second.when < oldestKept;
        });
    }
    m_dirty |= m_entries.size() != before;
}

bool EventJournal::load(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&f);
    quint32 magic = 0;
    quint32 count = 0;
    stream >> magic >> count;
    if (magic != s_magic || count > 10 * s_keepCount) {
        return false;
    }
    QHash<QString, Entry> loaded;
    for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; ++i) {
        QString path;
        Entry entry;
        stream >> path >> entry.when >> entry.kinds >> entry.count >> entry.program >> entry.app;
        if (m_tree->shown(path.left(path.lastIndexOf(QLatin1Char('/'))))) { // (still inside a home)
            loaded.insert(path, entry);
        }
    }
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    m_entries = loaded;
    return true;
}

bool EventJournal::save(const QString &file) const
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // root only: it lists the users' files
    QDataStream stream(&f);
    stream << s_magic << quint32(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        stream << it.key() << it->when << it->kinds << it->count << it->program << it->app;
    }
    if (!f.commit()) {
        return false;
    }
    m_dirty = false;
    return true;
}

bool EventJournal::isDirty() const
{
    return m_dirty;
}
