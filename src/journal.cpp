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
constexpr quint32 s_magic = 0x4e474a32; // "NGJ2": the entries, the operations, the next operation's number
constexpr qint64 s_keepMs = 14LL * 24 * 3600 * 1000;
constexpr int s_keepCount = 20000;
constexpr int s_keepOperations = 5000;
// an operation: files changed one after the other, each within this long of the one before
constexpr qint64 s_operationGapMs = 10 * 1000;
// the files of one operation that have an entry of their own; the rest are counted
constexpr quint32 s_operationEntries = 300;

template<typename Hash, typename When>
void dropOldest(Hash &hash, int most, When when)
{
    if (hash.size() <= most) {
        return;
    }
    QList<qint64> times;
    times.reserve(hash.size());
    for (auto it = hash.cbegin(); it != hash.cend(); ++it) {
        times.append(when(it.value()));
    }
    std::nth_element(times.begin(), times.begin() + (times.size() - most), times.end());
    const qint64 oldestKept = times.at(times.size() - most);
    for (auto it = hash.begin(); it != hash.end();) {
        it = when(it.value()) < oldestKept ? hash.erase(it) : std::next(it);
    }
    // (several as old as the oldest kept: as many of those as it takes go too)
    for (auto it = hash.begin(); it != hash.end() && hash.size() > most;) {
        it = when(it.value()) == oldestKept ? hash.erase(it) : std::next(it);
    }
}

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
                                                             "\\.(?:o|obj|d|lo|gch|pch|pyc|pyo|class)$|" // built from something else
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

bool EventJournal::wanted(const QString &folder, const QString &name, bool isFolder) const
{
    if (isNoise(name) || !m_tree->shown(folder) || m_tree->excludedName(folder, name)) {
        return false;
    }
    // what is inside the folders build systems fill, at any depth (a folder by that name, not a file: "build-notes.txt")
    static const QRegularExpression built(QStringLiteral("/(?:build|_build|build-[^/]*|cmake-build-[^/]*|CMakeFiles|[^/]*_autogen|node_modules|__pycache__)/"));
    const QString home = m_tree->homeRootOf(folder);
    return !built.match(QStringView(folder).mid(home.size()).toString() + QLatin1Char('/') + (isFolder ? name + QLatin1Char('/') : QString())).hasMatch();
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

quint64 EventJournal::operationFor(const Who &by, const QString &path, bool isFolder, qint64 when, quint32 kinds, bool *keepEntry)
{
    *keepEntry = true;
    // who is at work: an application, whatever its processes; what belongs to none (compilers, shells, tools)
    // together. Each home apart: nothing of one user's shows through another's
    const QString home = m_tree->homeRootOf(path);
    const QString key = (by.app.isEmpty() ? QStringLiteral("tools:") : QStringLiteral("app:") + by.app + QLatin1Char(':')) + home;
    if (m_live.size() > 1024) {
        m_live.clear();
    }
    Live &live = m_live[key];
    auto it = m_operations.find(live.id);
    if (live.id == 0 || it == m_operations.end() || when - it->last > s_operationGapMs) {
        if (isFolder) {
            return 0; // (a folder made alone begins nothing; one made while files arrive is part of what is going on)
        }
        live.id = m_nextOperation++;
        live.counted.clear();
        it = m_operations.insert(live.id, Operation{home, QString(), when, when, 0, 0, by.program, by.app});
    }
    Operation &operation = *it;
    operation.last = when;
    operation.kinds |= kinds & ~quint32(FileEvent::Folder);
    if (operation.program.isEmpty()) {
        operation.program = by.program;
    }
    m_dirty = true;
    if (isFolder) {
        return live.id;
    }
    const QString folder = path.left(path.lastIndexOf(QLatin1Char('/')));
    operation.folder = operation.count == 0 ? folder : commonFolder(operation.folder, folder);
    if (live.counted.size() > 500000) {
        live.counted.clear();
    }
    const size_t hash = qHash(path);
    if (!live.counted.contains(hash)) {
        live.counted.insert(hash);
        ++operation.count;
        // past a number of files the rest are counted only: a thousand lines of one unpacking help nobody, and
        // would push everything else out of the journal
        *keepEntry = operation.count <= s_operationEntries;
    }
    return live.id;
}

void EventJournal::put(const QString &path, quint32 kinds, int pid, qint64 when)
{
    const Who by = who(pid, when);
    bool keepEntry = true;
    const quint64 operation = operationFor(by, path, kinds & FileEvent::Folder, when, kinds, &keepEntry);
    auto known = m_entries.find(path);
    if (!keepEntry && known == m_entries.end()) {
        return;
    }
    Entry &entry = known != m_entries.end() ? *known : m_entries[path];
    // what is created and then written to within moments is still "created" (a copy, a download, a new document)
    const bool sameBurst = entry.when > 0 && when - entry.when < 60 * 1000;
    const quint32 before = entry.kinds;
    entry.kinds = (sameBurst ? (before & (FileEvent::Created | FileEvent::Moved)) : 0) | kinds;
    entry.when = when;
    if (operation != 0) {
        entry.operation = operation;
    }
    // (within those moments the one who made it stays: a program that touches it right after did not make it)
    const bool madeBefore = sameBurst && (before & FileEvent::Created) && !(kinds & FileEvent::Created) && !entry.program.isEmpty();
    if (!madeBefore && (!by.program.isEmpty() || !sameBurst)) {
        entry.program = by.program;
        entry.app = by.app;
    }
    m_dirty = true;
}

void EventJournal::record(const QString &folder, const QString &name, quint32 kinds, int pid, qint64 when)
{
    if (wanted(folder, name, kinds & FileEvent::Folder)) {
        put(folder + QLatin1Char('/') + name, kinds, pid, when);
    }
}

void EventJournal::remove(const QString &folder, const QString &name, bool isFolder)
{
    const QString path = folder + QLatin1Char('/') + name;
    m_dirty |= m_entries.remove(path) > 0;
    m_temps.remove(path);
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
    const bool had = m_entries.contains(oldPath);
    const Entry old = m_entries.take(oldPath);
    const Temp temp = m_temps.take(oldPath);
    m_dirty |= had; // (also when the new name is one nothing is kept for)
    const bool kept = wanted(newFolder, newName, isFolder);
    if (isFolder) {
        // what was inside is under the new name now (or out of sight: then it is dropped)
        const QString prefix = oldPath + QLatin1Char('/');
        QHash<QString, Entry> moved;
        const auto gone = m_entries.removeIf([&](const std::pair<const QString &, Entry &> &entry) {
            if (!entry.first.startsWith(prefix)) {
                return false;
            }
            if (kept) {
                moved.insert(newPath + entry.first.mid(oldPath.size()), entry.second);
            }
            return true;
        });
        m_dirty |= gone > 0;
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

void EventJournal::prune()
{
    const auto gone = m_entries.removeIf([this](const std::pair<const QString &, Entry &> &entry) {
        const int slash = entry.first.lastIndexOf(QLatin1Char('/'));
        return !wanted(entry.first.left(slash), entry.first.mid(slash + 1), entry.second.kinds & FileEvent::Folder);
    });
    m_dirty |= gone > 0;
}

QList<FileEvent> EventJournal::events(uint uid, qint64 since, int limit) const
{
    QList<FileEvent> list;
    if (limit <= 0) {
        return list;
    }
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->when < since) {
            continue;
        }
        const QString path = m_tree->named(uid, it.key());
        if (!path.isEmpty()) {
            list.append({path, it->when, it->kinds, it->program, it->app, it->operation});
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

QList<FileOperation> EventJournal::operations(uint uid, qint64 since, int atLeast, int limit) const
{
    QList<FileOperation> list;
    const QString home = m_tree->realHomeOf(uid);
    if (limit <= 0 || home.isEmpty()) {
        return list;
    }
    for (auto it = m_operations.cbegin(); it != m_operations.cend(); ++it) {
        if (it->last < since || it->home != home || int(it->count) < atLeast) {
            continue;
        }
        const QString folder = m_tree->named(uid, it->folder);
        if (!folder.isEmpty()) {
            list.append({it.key(), folder, it->first, it->last, it->count, it->kinds, it->program, it->app});
        }
    }
    std::sort(list.begin(), list.end(), [](const FileOperation &a, const FileOperation &b) {
        return a.last > b.last;
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
    const auto before = m_entries.size() + m_operations.size();
    m_entries.removeIf([now](const std::pair<const QString &, Entry &> &entry) {
        return now - entry.second.when > s_keepMs;
    });
    dropOldest(m_entries, s_keepCount, [](const Entry &entry) {
        return entry.when;
    });
    // an operation of one file says nothing more than its file: only the ones of several are kept past their course
    m_operations.removeIf([now](const std::pair<const quint64 &, Operation &> &operation) {
        return now - operation.second.last > s_keepMs || (operation.second.count < 2 && now - operation.second.last > s_operationGapMs);
    });
    dropOldest(m_operations, s_keepOperations, [](const Operation &operation) {
        return operation.last;
    });
    m_dirty |= m_entries.size() + m_operations.size() != before;
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
    QHash<QString, Entry> entries;
    for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; ++i) {
        QString path;
        Entry entry;
        stream >> path >> entry.when >> entry.kinds >> entry.operation >> entry.program >> entry.app;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        // (still inside a home, and not something left out since)
        if (slash > 0 && wanted(path.left(slash), path.mid(slash + 1), entry.kinds & FileEvent::Folder)) {
            entries.insert(path, entry);
        }
    }
    quint32 operationCount = 0;
    quint64 next = 1;
    stream >> operationCount;
    if (operationCount > 10 * s_keepOperations) {
        return false;
    }
    QHash<quint64, Operation> operations;
    for (quint32 i = 0; i < operationCount && stream.status() == QDataStream::Ok; ++i) {
        quint64 id = 0;
        Operation operation;
        stream >> id >> operation.home >> operation.folder >> operation.first >> operation.last >> operation.count >> operation.kinds >> operation.program >> operation.app;
        if (m_tree->homeRootOf(operation.folder) == operation.home && !operation.home.isEmpty()) {
            operations.insert(id, operation);
        }
    }
    stream >> next;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    m_entries = entries;
    m_operations = operations;
    m_nextOperation = qMax<quint64>(next, 1);
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
        stream << it.key() << it->when << it->kinds << it->operation << it->program << it->app;
    }
    stream << quint32(m_operations.size());
    for (auto it = m_operations.cbegin(); it != m_operations.cend(); ++it) {
        stream << it.key() << it->home << it->folder << it->first << it->last << it->count << it->kinds << it->program << it->app;
    }
    stream << m_nextOperation;
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
