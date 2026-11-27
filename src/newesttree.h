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
        QString path; // as the account names it, no trailing slash
        QString real; // where it really is (the home may be a link): what the kernel reports
    };

    /** The homes of the human users (uid >= UID_MIN in /etc/login.defs, 1000 if not set). */
    static QList<Home> userHomes();

    explicit NewestTree(const QList<Home> &homes);

    /** Something changed in @p folder at @p when (seconds since the epoch). Returns false when it was not kept:
     *  outside every home, or inside a hidden folder (like a file manager, which hides them; .git still counts). */
    bool touch(const QString &folder, qint64 when);
    /** Everything is to be considered changed from @p when on (the kernel queue overflowed: events were lost).
     *  Also the folders nothing was recorded for yet. */
    void touchAll(qint64 when);

    /** Whether @p uid has a home here. */
    bool hasHome(uint uid) const;
    /** When something last changed under @p folder (0: nothing seen since the journal began). @p folder is named
     *  as the user names it (through the home as the account has it, or where it really is). @p allowed is false
     *  when the folder is not @p uid's: outside the home, or in another user's home that lies inside it. */
    qint64 newest(uint uid, const QString &folder, bool *allowed) const;
    /** The folders of @p uid's home that changed at @p since or later, at most @p limit of them, named through the
     *  home as the account has it. Never a folder of another user's home that lies inside it. */
    QStringList changedSince(uint uid, qint64 since, int limit) const;

    /** Whether @p real (a folder as the kernel reports it) is in a home and not inside a hidden folder: what a
     *  file manager shows. .git counts for the dates of the folders (@p withGit), not as something shown. */
    bool shown(const QString &real, bool withGit = false) const;
    /** @p real as @p uid names it (through the home as the account has it); empty when it is not @p uid's. */
    QString named(uint uid, const QString &real) const;

    bool load(const QString &file);
    bool save(const QString &file) const;
    bool isDirty() const;

private:
    /** The home @p real (a path as the kernel reports it) belongs to: the deepest one that holds it. */
    const Home *homeFor(const QString &real) const;
    const Home *homeOf(uint uid) const;

    QList<Home> m_homes;
    QHash<QString, qint64> m_newest; // real folder path -> seconds since the epoch
    qint64 m_overflow = 0; // events were lost then: nothing is older than this
    mutable bool m_dirty = false;
};
