// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QCache>
#include <QHash>
#include <QList>
#include <QSet>
#include <QObject>
#include <QString>

class QSocketNotifier;

enum FsWhat {
    FsCreated,
    FsWritten, // data was written (many times while a file is being written)
    FsClosedAfterWriting, // closed by a process that had it open for writing
    FsAttributes,
    FsDeleted,
    FsRenamed, // (kernels with FAN_RENAME, Linux 5.17)
    FsMovedAway, // (older kernels: the two halves of a rename, apart)
    FsMovedHere,
};

/** A line of /proc/self/mountinfo. */
struct MountEntry {
    QString root; // which folder of the filesystem is mounted ("/" or, on btrfs, the subvolume)
    QString point; // where
    QString type;
    QString source;
};
QList<MountEntry> mountTable();

/**
 * Listens to fanotify for a whole filesystem at a time (FAN_MARK_FILESYSTEM) and reports, for each change,
 * the folder it happened in and the name of the entry. Needs CAP_SYS_ADMIN (the mark) and CAP_DAC_READ_SEARCH
 * (turning the folder's file handle back into a path), so it runs as root.
 */
class FanotifyWatcher : public QObject
{
    Q_OBJECT

public:
    /** @p privateDir: a folder of the service's own, where it may mount (see watch()); empty: it never mounts. */
    explicit FanotifyWatcher(const QString &privateDir = QString(), QObject *parent = nullptr);
    ~FanotifyWatcher() override;

    /** Starts watching the filesystem that holds @p path. Several paths on one filesystem share one mark.
     *  Nothing is kept open on it: a disk mounted inside a home can still be unmounted.
     *
     *  btrfs refuses the mark on a subvolume (EXDEV), and a home is very often one. With @p mayMount, the top of
     *  that btrfs filesystem is then mounted read-only in the service's private folder and marked there; the paths
     *  of the events are given as they are under the mount the caller named. That mount keeps the filesystem
     *  busy, so it is for the filesystem of a home, not for a removable disk mounted inside one. */
    bool watch(const QString &path, QString *error = nullptr, bool mayMount = false);

    bool isValid() const;
    /** How many files have their writes held back at the moment (see the private part). */
    int heldBack() const;

Q_SIGNALS:
    /** Something called @p name changed in @p directory: modified, attributes changed, created, deleted or moved. */
    void changed(const QString &directory, const QString &name);
    /** The same, told apart: @p what is one of the Fs* values. For FsRenamed, @p folder and @p name are the new
     *  place and @p oldFolder and @p oldName the one before (either pair may be empty when that side could not be
     *  resolved). @p pid is the process that did it (0 when the kernel does not say). */
    void fileEvent(int what, const QString &folder, const QString &name, const QString &oldFolder, const QString &oldName, bool isFolder, int pid);
    /** The kernel queue overflowed: events were lost, and every folder should be treated as changed. */
    void overflowed();

private:
    void readEvents();
    bool watchBtrfsTop(const QString &path, quint64 key, QString *error);
    QString translated(const QString &path) const;

    struct Rewrite {
        QString from; // the subvolume as seen under the private mount of the filesystem's top
        QString to; // the mount point it is known by
    };
    QString m_privateDir;
    QList<Rewrite> m_rewrites;
    QStringList m_ownMounts; // unmounted at the end
    QSet<quint64> m_viaTop; // subvolumes watched through the top of their filesystem

    void coolDown();

    // A file written to without pause (the disk of a virtual machine, a database, a log) would wake the service
    // for every write. Past a number of writes in a second the kernel is told to keep that file's writes to
    // itself for a while (an ignore mark on the file); its closing is still told.
    QHash<size_t, int> m_writes; // the writes of this second, by file
    qint64 m_writesSecond = 0;
    QList<QByteArray> m_ignored; // the files whose writes are not told for now
    // folder handles turned into paths lately (asked of the kernel once, not for every event)
    QCache<QByteArray, QString> m_paths{8192};
    quint64 m_mask = 0; // what is asked of each filesystem (decided at the first mark: FAN_RENAME or not)
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<quint64, QByteArray> m_roots; // filesystem id -> a folder on it, opened while events are read to resolve file handles
};
