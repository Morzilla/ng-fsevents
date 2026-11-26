// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QSocketNotifier;

/**
 * Listens to fanotify for a whole filesystem at a time (FAN_MARK_FILESYSTEM) and reports, for each change,
 * the folder it happened in and the name of the entry. Needs CAP_SYS_ADMIN (the mark) and CAP_DAC_READ_SEARCH
 * (turning the folder's file handle back into a path), so it runs as root.
 */
class FanotifyWatcher : public QObject
{
    Q_OBJECT

public:
    explicit FanotifyWatcher(QObject *parent = nullptr);
    ~FanotifyWatcher() override;

    /** Starts watching the filesystem that holds @p path. Several paths on one filesystem share one mark. */
    bool watch(const QString &path, QString *error = nullptr);

    bool isValid() const;

Q_SIGNALS:
    /** Something called @p name changed in @p directory: modified, attributes changed, created, deleted or moved. */
    void changed(const QString &directory, const QString &name);
    /** The kernel queue overflowed: events were lost, and every folder should be treated as changed. */
    void overflowed();

private:
    void readEvents();

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<quint64, int> m_mountFds; // filesystem id -> an open folder on it, to resolve file handles
};
