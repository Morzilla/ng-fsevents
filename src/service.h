// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QDBusContext>
#include <QObject>
#include <QStringList>

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
    explicit FsEventsService(NewestTree *tree, EventJournal *journal, QObject *parent = nullptr);

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
    /** The daemon's clock, to ask ChangedSince() from later on. */
    qlonglong Now();

private:
    bool callerUid(uint *uid);

    NewestTree *m_tree;
    EventJournal *m_journal;
};
