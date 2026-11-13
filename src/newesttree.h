// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * For each user's home: when something last changed under each folder, at any depth. A change in a folder
 * moves the date of that folder and of every folder above it, up to the home. Changes outside the homes are
 * dropped at once and never kept.
 */
class NewestTree
{
public:
    struct Home {
        uint uid = 0;
        QString path; // no trailing slash
    };

    /** The homes of the human users (uid >= UID_MIN in /etc/login.defs, 1000 if not set). */
    static QList<Home> userHomes();

    explicit NewestTree(const QList<Home> &homes);

    /** Something changed in @p folder at @p when (seconds since the epoch). */
    void touch(const QString &folder, qint64 when);
    /** Everything is to be considered changed (the kernel queue overflowed). */
    void touchAll(qint64 when);

    /** The home of @p uid, or an empty string. */
    QString homeOf(uint uid) const;
    /** When something last changed under @p folder (0: nothing seen since the journal began). */
    qint64 newest(const QString &folder) const;
    /** The folders of @p home that changed after @p since, at most @p limit of them. */
    QStringList changedSince(const QString &home, qint64 since, int limit) const;

    bool load(const QString &file);
    bool save(const QString &file) const;
    bool isDirty() const;

private:
    const Home *homeFor(const QString &folder) const;

    QList<Home> m_homes;
    QHash<QString, qint64> m_newest; // absolute folder path -> seconds since the epoch
    mutable bool m_dirty = false;
};
