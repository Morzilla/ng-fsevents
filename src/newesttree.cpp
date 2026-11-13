// SPDX-License-Identifier: LGPL-2.1-or-later
#include "newesttree.h"

#include <QDataStream>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>

#include <pwd.h>

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

constexpr quint32 s_magic = 0x4e474645; // "NGFE"
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
        homes.append({entry->pw_uid, path});
    }
    endpwent();
    return homes;
}

NewestTree::NewestTree(const QList<Home> &homes)
    : m_homes(homes)
{
}

const NewestTree::Home *NewestTree::homeFor(const QString &folder) const
{
    const Home *best = nullptr;
    for (const Home &home : m_homes) {
        if (isUnder(folder, home.path) && (!best || home.path.size() > best->path.size())) {
            best = &home;
        }
    }
    return best;
}

QString NewestTree::homeOf(uint uid) const
{
    for (const Home &home : m_homes) {
        if (home.uid == uid) {
            return home.path;
        }
    }
    return {};
}

void NewestTree::touch(const QString &folder, qint64 when)
{
    const Home *home = homeFor(folder);
    if (!home) {
        return; // not in a user's home: not kept
    }
    QString current = folder;
    for (;;) {
        qint64 &value = m_newest[current];
        if (value >= when) {
            break; // the folders above are at least as recent already
        }
        value = when;
        m_dirty = true;
        if (current == home->path) {
            break;
        }
        current = current.left(current.lastIndexOf(QLatin1Char('/')));
        if (current.isEmpty()) {
            break;
        }
    }
}

void NewestTree::touchAll(qint64 when)
{
    for (auto it = m_newest.begin(); it != m_newest.end(); ++it) {
        it.value() = when;
    }
    for (const Home &home : std::as_const(m_homes)) {
        m_newest[home.path] = when;
    }
    m_dirty = true;
}

qint64 NewestTree::newest(const QString &folder) const
{
    return m_newest.value(folder, 0);
}

QStringList NewestTree::changedSince(const QString &home, qint64 since, int limit) const
{
    QStringList folders;
    for (auto it = m_newest.cbegin(); it != m_newest.cend() && folders.size() < limit; ++it) {
        if (it.value() > since && isUnder(it.key(), home)) {
            folders.append(it.key());
        }
    }
    return folders;
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
    stream >> loaded;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    // only what is still inside a home
    for (auto it = loaded.cbegin(); it != loaded.cend(); ++it) {
        if (homeFor(it.key())) {
            m_newest.insert(it.key(), it.value());
        }
    }
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
    stream << s_magic << m_newest;
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
