// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QDBusContext>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

#include "journal.h"

class EventJournal;
class NewestTree;

/**
 * org.ng.FsEvents1 on the system bus. Every answer is about the caller's own home only: the caller is the uid
 * the bus reports for the connection (it cannot be faked), and any folder outside that home is refused.
 * Newest and ChangedSince give folders and dates only; Events gives the caller the names of its own files and
 * of the programs that changed them.
 */
class FsEventsService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ng.FsEvents1")

public:
    /** @p whyNotWatched: for a real path, why its filesystem is not listened to; empty when it is. */
    FsEventsService(NewestTree *tree, EventJournal *journal, std::function<QString(const QString &)> whyNotWatched, std::function<QVariantMap()> load,
                    QObject *parent = nullptr);

public Q_SLOTS:
    /** When something last changed under @p folder, in seconds since the epoch (0: nothing seen yet). */
    qlonglong Newest(const QString &folder);
    /** The folders of the caller's home that changed at @p since or later (seconds since the epoch), at most 1000.
     *  The second asked for is included, so nothing that happens later in it is missed: a folder may be given
     *  again by the next call. */
    QStringList ChangedSince(qlonglong since);
    /** The latest that happened to each file and folder of the caller's home, at @p since (milliseconds since the
     *  epoch) or later, newest first, at most @p limit (and never more than 2000). Each is (path, when in
     *  milliseconds, kinds, program, application, count): see FileEvent. One entry per path. */
    QList<FileEvent> Events(qlonglong since, uint limit);
    /**
     * How the service is doing for the caller: "watched" (b: the caller's home is listened to), "reason" (s: why
     * not, in the kernel's words), "files" (u: entries of the journal of the files), "started" (x: since when it
     * runs, seconds), "overflowed" (x: when events were last lost, 0 never), "days" (u: how long the journal of
     * the files keeps an entry), "version" (s); and how busy it is: "events" (t: changes heard since it started,
     * anywhere on the filesystems listened to), "held" (u: files written to without pause whose writes are held back).
     */
    QVariantMap Status();
    /** The daemon's clock, to ask ChangedSince() from later on. */
    qlonglong Now();

private:
    bool callerUid(uint *uid);

    NewestTree *m_tree;
    EventJournal *m_journal;
    std::function<QString(const QString &)> m_whyNotWatched;
    std::function<QVariantMap()> m_load;
    qint64 m_started;
};
