// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QDBusContext>
#include <QObject>
#include <QStringList>

class NewestTree;

/**
 * org.ng.FsEvents1 on the system bus. Every answer is about the caller's own home only: the caller is the uid
 * the bus reports for the connection (it cannot be faked), and any folder outside that home is refused.
 * No file names are ever sent, only folders of the caller's home and dates.
 */
class FsEventsService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ng.FsEvents1")

public:
    explicit FsEventsService(NewestTree *tree, QObject *parent = nullptr);

public Q_SLOTS:
    /** When something last changed under @p folder, in seconds since the epoch (0: nothing seen yet). */
    qlonglong Newest(const QString &folder);
    /** The folders of the caller's home that changed after @p since (seconds since the epoch), at most 1000. */
    QStringList ChangedSince(qlonglong since);
    /** The daemon's clock, to ask ChangedSince() from later on. */
    qlonglong Now();

private:
    QString callerHome();

    NewestTree *m_tree;
};
