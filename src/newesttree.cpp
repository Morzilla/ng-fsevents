// SPDX-License-Identifier: LGPL-2.1-or-later
#include "newesttree.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
uint uidMin()
{
    QFile defs(QStringLiteral("/etc/login.defs"));
    if (defs.open(QIODevice::ReadOnly)) {
        static const QRegularExpression line(QStringLiteral("^\\s*UID_MIN\\s+(\\d+)"));
        while (!defs.atEnd()) {
            const auto match = line.match(QString::fromUtf8(defs.readLine()));
            if (match.hasMatch()) {
                return match.captured(1).toUInt();
            }
        }
    }
    return 1000;
}

bool isUnder(const QString &path, const QString &root)
{
    return path == root || (path.startsWith(root) && path.at(root.size()) == QLatin1Char('/'));
}

constexpr quint32 s_magic = 0x4e474632; // "NGF2": the map of the folders, then when events were last lost
}

QList<NewestTree::Home> NewestTree::userHomes()
{
    QList<Home> homes;
    const uint minimum = uidMin();
    setpwent();
    while (passwd *entry = getpwent()) {
        if (entry->pw_uid < minimum || entry->pw_uid == 65534 || !entry->pw_dir || entry->pw_dir[0] != '/') {
            continue; // system accounts and nobody
        }
        QString path = QFile::decodeName(entry->pw_dir);
        while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }
        // the kernel reports where a folder really is: a home that is a link is known by both names
        const QString real = QFileInfo(path).canonicalFilePath();
        homes.append({entry->pw_uid, path, real.isEmpty() ? path : real, {}, {}});
    }
    endpwent();
    return homes;
}

NewestTree::NewestTree(const QList<Home> &homes)
    : m_homes(homes)
{
}

const NewestTree::Home *NewestTree::homeFor(const QString &real) const
{
    const Home *best = nullptr;
    for (const Home &home : m_homes) {
        if (isUnder(real, home.real) && (!best || home.real.size() > best->real.size())) {
            best = &home;
        }
    }
    return best;
}

const NewestTree::Home *NewestTree::homeOf(uint uid) const
{
    for (const Home &home : m_homes) {
        if (home.uid == uid) {
            return &home;
        }
    }
    return nullptr;
}

bool NewestTree::hasHome(uint uid) const
{
    return homeOf(uid) != nullptr;
}

namespace
{
QByteArray readExclusions(const QString &file, bool ownedBy, uint uid)
{
    // (never a link, a device or a pipe; the user's own file is the user's)
    const int fd = ::open(QFile::encodeName(file).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    QByteArray text;
    struct stat info;
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= 64 * 1024 && (!ownedBy || info.st_uid == uid)) {
        text.resize(info.st_size);
        const ssize_t n = ::read(fd, text.data(), text.size());
        text.truncate(qMax<ssize_t>(0, n));
    }
    ::close(fd);
    return text;
}
}

void NewestTree::loadExclusions()
{
    const QByteArray everyone = readExclusions(QStringLiteral("/etc/ng-fsevents/exclude"), false, 0);
    for (Home &home : m_homes) {
        home.excludedNames.clear();
        home.excludedFolders.clear();
        const QByteArray own = readExclusions(home.real + QStringLiteral("/.config/ng-fsevents/exclude"), true, home.uid);
        // (left out for everyone: the disks of virtual machines, written to all the time and never "a recent file")
        static const QByteArray always("*.qcow2\n*.qcow\n*.vmdk\n*.vdi\n*.vhd\n*.vhdx\n");
        const QStringList lines = QString::fromUtf8(always + everyone + '\n' + own).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (QString line : lines) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.size() > 1024 || home.excludedNames.size() + home.excludedFolders.size() >= 206) {
                continue;
            }
            if (line.contains(QLatin1Char('/'))) {
                // a folder: of the home, unless the path is absolute ("~/" is the home too)
                if (line.startsWith(QLatin1String("~/"))) {
                    line.remove(0, 2);
                }
                QString folder = QDir::cleanPath(line.startsWith(QLatin1Char('/')) ? line : home.real + QLatin1Char('/') + line);
                if (home.path != home.real && isUnder(folder, home.path)) {
                    folder = home.real + folder.mid(home.path.size());
                }
                if (isUnder(folder, home.real) && folder != home.real) {
                    home.excludedFolders.append(folder);
                }
            } else {
                const QRegularExpression pattern = QRegularExpression::fromWildcard(line, Qt::CaseInsensitive);
                if (pattern.isValid()) {
                    home.excludedNames.append(pattern);
                }
            }
        }
    }
}

bool NewestTree::isExclusionFile(const QString &real, const QString &name) const
{
    if (name != QLatin1String("exclude") || !real.endsWith(QLatin1String("/.config/ng-fsevents"))) {
        return false;
    }
    const Home *home = homeFor(real);
    return home && real == home->real + QLatin1String("/.config/ng-fsevents");
}

bool NewestTree::excludedName(const QString &real, const QString &name) const
{
    const Home *home = homeFor(real);
    if (!home) {
        return false;
    }
    for (const QRegularExpression &pattern : home->excludedNames) {
        if (pattern.match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

int NewestTree::exclusions(uint uid) const
{
    const Home *home = homeOf(uid);
    return home ? home->excludedNames.size() + home->excludedFolders.size() : 0;
}

bool NewestTree::shown(const QString &real, bool withGit) const
{
    const Home *home = homeFor(real);
    if (!home) {
        return false;
    }
    for (const QString &folder : home->excludedFolders) {
        if (isUnder(real, folder)) {
            return false; // left out by the user
        }
    }
    const QStringView inside = QStringView(real).mid(home->real.size());
    for (const QStringView part : inside.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1Char('.')) && !(withGit && part == QLatin1String(".git"))) {
            return false;
        }
    }
    return true;
}

QString NewestTree::named(uint uid, const QString &real) const
{
    const Home *own = homeOf(uid);
    const Home *owner = homeFor(real);
    if (!own || !owner || owner->real != own->real) {
        return {};
    }
    return own->path + real.mid(own->real.size());
}

bool NewestTree::touch(const QString &folder, qint64 when)
{
    // not in a user's home: not kept. Inside a hidden folder (caches, settings, containers...): what a file
    // manager does not show does not count
    if (!shown(folder, true)) {
        return false;
    }
    const Home *home = homeFor(folder);
    QString current = folder;
    for (;;) {
        qint64 &value = m_newest[current];
        if (value >= when) {
            break; // the folders above are at least as recent already
        }
        value = when;
        m_dirty = true;
        if (current == home->real) {
            break;
        }
        current = current.left(current.lastIndexOf(QLatin1Char('/')));
        if (current.isEmpty()) {
            break;
        }
    }
    return true;
}

void NewestTree::touchAll(qint64 when)
{
    // what was lost may be about any folder, also one nothing is recorded for: one date that every answer is
    // held against, not a date on each entry
    if (when > m_overflow) {
        m_overflow = when;
        m_dirty = true;
    }
}

qint64 NewestTree::newest(uint uid, const QString &folder, bool *allowed) const
{
    *allowed = false;
    const Home *own = homeOf(uid);
    if (!own) {
        return 0;
    }
    QString real = QDir::cleanPath(folder);
    if (own->path != own->real && isUnder(real, own->path)) {
        real = own->real + real.mid(own->path.size()); // asked through the home's link
    }
    // the deepest home that holds it must be the caller's: another user's home may lie inside this one
    const Home *owner = homeFor(real);
    if (!owner || owner->real != own->real) {
        return 0;
    }
    *allowed = true;
    return qMax(m_newest.value(real, 0), m_overflow);
}

QStringList NewestTree::changedSince(uint uid, qint64 since, int limit) const
{
    QStringList folders;
    const Home *own = homeOf(uid);
    if (!own) {
        return folders;
    }
    if (m_overflow >= since) {
        return {own->path}; // events were lost since then: the whole home counts as changed
    }
    for (auto it = m_newest.cbegin(); it != m_newest.cend() && folders.size() < limit; ++it) {
        if (it.value() < since) {
            continue;
        }
        const Home *owner = homeFor(it.key());
        if (owner && owner->real == own->real) {
            folders.append(own->path + it.key().mid(own->real.size()));
        }
    }
    return folders;
}

QString NewestTree::homeRootOf(const QString &real) const
{
    const Home *home = homeFor(real);
    return home ? home->real : QString();
}

QString NewestTree::realHomeOf(uint uid) const
{
    const Home *home = homeOf(uid);
    return home ? home->real : QString();
}

qint64 NewestTree::overflowedAt() const
{
    return m_overflow;
}

void NewestTree::expire(qint64 now)
{
    constexpr qint64 keep = 180LL * 24 * 3600;
    constexpr int most = 200000;
    const auto before = m_newest.size();
    m_newest.removeIf([now](const std::pair<const QString &, qint64 &> &entry) {
        return now - entry.second > keep;
    });
    if (m_newest.size() > most) {
        QList<qint64> times = m_newest.values();
        std::nth_element(times.begin(), times.begin() + (times.size() - most), times.end());
        const qint64 oldestKept = times.at(times.size() - most);
        m_newest.removeIf([oldestKept](const std::pair<const QString &, qint64 &> &entry) {
            return entry.second < oldestKept;
        });
    }
    m_dirty |= m_newest.size() != before;
}

bool NewestTree::load(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&f);
    quint32 magic = 0;
    stream >> magic;
    if (magic != s_magic) {
        return false;
    }
    QHash<QString, qint64> loaded;
    qint64 overflow = 0;
    stream >> loaded >> overflow;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    // only what is still inside a home
    for (auto it = loaded.cbegin(); it != loaded.cend(); ++it) {
        if (homeFor(it.key())) {
            m_newest.insert(it.key(), it.value());
        }
    }
    m_overflow = overflow;
    return true;
}

bool NewestTree::save(const QString &file) const
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // root only: it lists folder names
    QDataStream stream(&f);
    stream << s_magic << m_newest << m_overflow;
    if (!f.commit()) {
        return false;
    }
    m_dirty = false;
    return true;
}

bool NewestTree::isDirty() const
{
    return m_dirty;
}
