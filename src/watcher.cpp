// SPDX-License-Identifier: LGPL-2.1-or-later
#include "watcher.h"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace
{
quint64 fsidKey(const __kernel_fsid_t &fsid)
{
    return (quint64(quint32(fsid.val[0])) << 32) | quint32(fsid.val[1]);
}

// (kernel headers older than 5.17)
#ifndef FAN_RENAME
#define FAN_RENAME 0x10000000
#endif
#ifndef FAN_EVENT_INFO_TYPE_OLD_DFID_NAME
#define FAN_EVENT_INFO_TYPE_OLD_DFID_NAME 10
#endif
#ifndef FAN_EVENT_INFO_TYPE_NEW_DFID_NAME
#define FAN_EVENT_INFO_TYPE_NEW_DFID_NAME 12
#endif

// what changes the date of a folder's contents, and the closing of a written file (when a program is done with it)
constexpr uint64_t s_base = FAN_MODIFY | FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_CREATE | FAN_DELETE | FAN_ONDIR;
constexpr uint64_t s_halves = FAN_MOVED_FROM | FAN_MOVED_TO;
}

QList<MountEntry> mountTable()
{
    // mount id, parent id, major:minor, root, mount point, options, optional fields..., "-", type, source, ...
    // (spaces and the like in a path are written \ooo)
    const auto unescaped = [](const QByteArray &raw) {
        QByteArray text;
        for (int i = 0; i < raw.size(); ++i) {
            if (raw.at(i) == '\\' && i + 3 < raw.size()) {
                text.append(char(raw.mid(i + 1, 3).toInt(nullptr, 8)));
                i += 3;
            } else {
                text.append(raw.at(i));
            }
        }
        return QFile::decodeName(text);
    };
    QList<MountEntry> table;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) {
        return table;
    }
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (fields.size() < 5 || dash < 5 || dash + 2 >= fields.size()) {
            continue;
        }
        table.append({unescaped(fields.at(3)), unescaped(fields.at(4)), QString::fromLatin1(fields.at(dash + 1)), unescaped(fields.at(dash + 2))});
    }
    return table;
}

FanotifyWatcher::FanotifyWatcher(const QString &privateDir, QObject *parent)
    : QObject(parent)
    , m_privateDir(privateDir)
{
    // FAN_REPORT_DFID_NAME (Linux 5.9): each event says in which folder (as a file handle) and under which name
    m_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_DFID_NAME, O_RDONLY | O_LARGEFILE);
    if (m_fd < 0) {
        return;
    }
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &FanotifyWatcher::readEvents);
    // the files held back are listened to again every minute: one that calmed down is told as before, one that
    // did not is held back again after a few writes (so the date of its folder still moves, a minute at a time)
    auto *cool = new QTimer(this);
    connect(cool, &QTimer::timeout, this, &FanotifyWatcher::coolDown);
    cool->start(60 * 1000);
}

FanotifyWatcher::~FanotifyWatcher()
{
    for (const QString &mount : std::as_const(m_ownMounts)) {
        const QByteArray local = QFile::encodeName(mount);
        ::umount2(local.constData(), MNT_DETACH);
        ::rmdir(local.constData());
    }
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

namespace
{
// FAN_RENAME gives both ends of a rename in one event; a kernel without it refuses the mask (EINVAL)
int addMark(int fd, quint64 &mask, const char *path)
{
    if (mask == 0) {
        if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, s_base | FAN_RENAME, AT_FDCWD, path) == 0) {
            mask = s_base | FAN_RENAME;
            return 0;
        }
        if (errno != EINVAL) {
            return -1;
        }
        if (fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, s_base | s_halves, AT_FDCWD, path) == 0) {
            mask = s_base | s_halves;
            return 0;
        }
        return -1;
    }
    return fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, path);
}
}

bool FanotifyWatcher::isValid() const
{
    return m_fd >= 0;
}

bool FanotifyWatcher::watch(const QString &path, QString *error, bool mayMount)
{
    const QByteArray local = QFile::encodeName(path);
    struct statfs sfs;
    if (m_fd < 0 || ::statfs(local.constData(), &sfs) != 0) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        return false;
    }
    __kernel_fsid_t fsid;
    std::memcpy(&fsid, &sfs.f_fsid, sizeof fsid);
    const quint64 key = fsidKey(fsid);
    if (m_roots.contains(key) || m_viaTop.contains(key)) {
        return true; // that filesystem is already watched
    }
    if (addMark(m_fd, m_mask, local.constData()) != 0) {
        // a btrfs subvolume has a filesystem id of its own, and the kernel refuses the mark there (seen on 7.0)
        if (errno == EXDEV && mayMount && !m_privateDir.isEmpty()) {
            return watchBtrfsTop(path, key, error);
        }
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        return false;
    }
    m_roots.insert(key, local);
    return true;
}

int FanotifyWatcher::heldBack() const
{
    return m_ignored.size();
}

void FanotifyWatcher::coolDown()
{
    for (const QByteArray &file : std::as_const(m_ignored)) {
        fanotify_mark(m_fd, FAN_MARK_REMOVE | FAN_MARK_IGNORED_MASK | FAN_MARK_IGNORED_SURV_MODIFY, FAN_MODIFY, AT_FDCWD, file.constData());
    }
    m_ignored.clear();
}

bool FanotifyWatcher::watchBtrfsTop(const QString &path, quint64 key, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    // the mount that holds the path: the deepest mount point above it
    MountEntry mount;
    const QList<MountEntry> table = mountTable();
    for (const MountEntry &entry : table) {
        const bool holds = path == entry.point || entry.point == QLatin1String("/") || path.startsWith(entry.point + QLatin1Char('/'));
        if (holds && entry.point.size() >= mount.point.size()) {
            mount = entry; // (a later line for the same point is mounted over the earlier one)
        }
    }
    if (mount.type != QLatin1String("btrfs")) {
        return fail(QStringLiteral("the mark is refused here, and it is not a btrfs subvolume"));
    }
    // the top of that filesystem (subvolume 5), read-only, where only this service sees it
    QString top;
    for (int n = 0; n < 64; ++n) {
        const QString candidate = m_privateDir + QStringLiteral("/top-%1").arg(n);
        if (::mkdir(QFile::encodeName(candidate).constData(), 0700) == 0) {
            top = candidate;
            break;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    if (top.isEmpty()) {
        return fail(QStringLiteral("no room in ") + m_privateDir);
    }
    const QByteArray localTop = QFile::encodeName(top);
    if (::mount(QFile::encodeName(mount.source).constData(), localTop.constData(), "btrfs", MS_RDONLY | MS_NOATIME | MS_NODEV | MS_NOSUID | MS_NOEXEC, "subvolid=5")
        != 0) {
        const QString why = QString::fromLocal8Bit(std::strerror(errno));
        ::rmdir(localTop.constData());
        return fail(QStringLiteral("cannot mount the top of the btrfs filesystem: ") + why);
    }
    m_ownMounts.append(top);
    struct statfs sfs;
    if (addMark(m_fd, m_mask, localTop.constData()) != 0 || ::statfs(localTop.constData(), &sfs) != 0) {
        return fail(QString::fromLocal8Bit(std::strerror(errno)));
    }
    __kernel_fsid_t fsid;
    std::memcpy(&fsid, &sfs.f_fsid, sizeof fsid);
    m_roots.insert(fsidKey(fsid), localTop);
    m_viaTop.insert(key);
    // what lies under <top>/<subvolume>/... is known to everyone as <mount point>/...
    const QString inside = QDir::cleanPath(path).mid(mount.point == QLatin1String("/") ? 0 : mount.point.size());
    const QString from = QDir::cleanPath(top + QLatin1Char('/') + mount.root + QLatin1Char('/') + inside);
    m_rewrites.append({from, QDir::cleanPath(path)});
    return true;
}

QString FanotifyWatcher::translated(const QString &path) const
{
    for (const Rewrite &rewrite : m_rewrites) {
        if (path == rewrite.from) {
            return rewrite.to;
        }
        if (path.startsWith(rewrite.from) && path.at(rewrite.from.size()) == QLatin1Char('/')) {
            return rewrite.to + path.mid(rewrite.from.size());
        }
    }
    return path;
}

void FanotifyWatcher::readEvents()
{
    alignas(fanotify_event_metadata) char buffer[64 * 1024];
    // a folder of each filesystem is open only while its events are read (an open folder would keep a disk
    // mounted inside a home from being unmounted)
    QHash<quint64, int> open;
    const auto closeAll = [&open] {
        for (int fd : std::as_const(open)) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };
    const auto rootFd = [this, &open](quint64 key) {
        auto it = open.find(key);
        if (it == open.end()) {
            const QByteArray root = m_roots.value(key);
            it = open.insert(key, root.isEmpty() ? -1 : ::open(root.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (*it < 0 && !root.isEmpty()) {
                m_roots.remove(key); // unmounted: marked again if it comes back
            }
        }
        return *it;
    };
    // a bounded number of buffers per call: under a steady stream of events the bus and the timers still get
    // their turn (the notifier calls again while there is more to read)
    for (int round = 0; round < 16; ++round) {
        ssize_t length = ::read(m_fd, buffer, sizeof buffer);
        if (length <= 0) {
            break; // EAGAIN: nothing more for now
        }
        auto *meta = reinterpret_cast<fanotify_event_metadata *>(buffer);
        for (; FAN_EVENT_OK(meta, length); meta = FAN_EVENT_NEXT(meta, length)) {
            if (meta->vers != FANOTIFY_METADATA_VERSION) {
                continue;
            }
            if (meta->mask & FAN_Q_OVERFLOW) {
                Q_EMIT overflowed();
                continue;
            }
            // the information records follow the metadata: the folder (as a file handle) and the name; a rename
            // has two, the old place and the new one
            QString folder, name, oldFolder, oldName;
            QString asMarked; // the folder as this service reaches it (under its own mount, for a btrfs subvolume)
            size_t fileKey = 0;
            const auto resolve = [&](const char *record, QString &intoFolder, QString &intoName) {
                const auto *fid = reinterpret_cast<const fanotify_event_info_fid *>(record);
                auto *handle = reinterpret_cast<file_handle *>(const_cast<unsigned char *>(fid->handle));
                const char *entry = reinterpret_cast<const char *>(handle->f_handle + handle->handle_bytes);
                // (the filesystem and the folder's handle name the folder; with the entry's name, the file)
                const QByteArray folderKey(reinterpret_cast<const char *>(&fid->fsid), int(sizeof fid->fsid + sizeof(file_handle) + handle->handle_bytes));
                fileKey = qHash(folderKey) ^ (qHash(QByteArrayView(entry)) * 31);
                if (const QString *known = m_paths.object(folderKey)) {
                    asMarked = *known;
                    intoFolder = translated(asMarked);
                    intoName = QFile::decodeName(entry);
                    return;
                }
                const int mountFd = rootFd(fsidKey(fid->fsid));
                if (mountFd < 0) {
                    return;
                }
                // the folder's handle back into a path (the folder may be gone already: ESTALE)
                const int dirFd = ::open_by_handle_at(mountFd, handle, O_PATH | O_CLOEXEC);
                if (dirFd < 0) {
                    return;
                }
                char target[4096];
                const QByteArray link = "/proc/self/fd/" + QByteArray::number(dirFd);
                const ssize_t n = ::readlink(link.constData(), target, sizeof target - 1);
                ::close(dirFd);
                if (n > 0) {
                    asMarked = QFile::decodeName(QByteArray(target, n));
                    m_paths.insert(folderKey, new QString(asMarked));
                    intoFolder = translated(asMarked);
                    intoName = QFile::decodeName(entry);
                }
            };
            const char *record = reinterpret_cast<const char *>(meta) + meta->metadata_len;
            const char *end = reinterpret_cast<const char *>(meta) + meta->event_len;
            while (record + sizeof(fanotify_event_info_header) <= end) {
                const auto *header = reinterpret_cast<const fanotify_event_info_header *>(record);
                if (header->len == 0) {
                    break;
                }
                if (header->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME || header->info_type == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME) {
                    resolve(record, folder, name);
                } else if (header->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME) {
                    resolve(record, oldFolder, oldName);
                }
                record += header->len;
            }
            // a folder renamed, moved or deleted: the paths kept for the handles below it are no longer true
            if ((meta->mask & FAN_ONDIR) && (meta->mask & (FAN_RENAME | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_DELETE))) {
                m_paths.clear();
            }
            // written to without pause: the kernel keeps this file's writes to itself for a while
            static const bool hold = !qEnvironmentVariableIsSet("NG_FSEVENTS_NO_HOLD"); // (to measure what it saves)
            if (hold && (meta->mask & FAN_MODIFY) && !(meta->mask & FAN_ONDIR) && !asMarked.isEmpty() && m_ignored.size() < 256) {
                const qint64 second = QDateTime::currentSecsSinceEpoch();
                if (second != m_writesSecond) {
                    m_writesSecond = second;
                    m_writes.clear();
                }
                if (++m_writes[fileKey] == 20) {
                    const QByteArray file = QFile::encodeName(asMarked + QLatin1Char('/') + name);
                    if (fanotify_mark(m_fd, FAN_MARK_ADD | FAN_MARK_IGNORED_MASK | FAN_MARK_IGNORED_SURV_MODIFY, FAN_MODIFY, AT_FDCWD, file.constData()) == 0) {
                        m_ignored.append(file);
                    }
                }
            }
            if (!oldFolder.isEmpty()) {
                Q_EMIT changed(oldFolder, oldName);
            }
            if (folder.isEmpty()) {
                if (meta->mask & FAN_RENAME && !oldFolder.isEmpty()) {
                    Q_EMIT fileEvent(FsMovedAway, oldFolder, oldName, {}, {}, meta->mask & FAN_ONDIR, meta->pid); // to where it cannot be followed
                }
                continue;
            }
            Q_EMIT changed(folder, name);
            // (one event may carry several kinds, merged by the kernel)
            const bool isFolder = meta->mask & FAN_ONDIR;
            const int pid = meta->pid;
            if (meta->mask & FAN_CREATE) {
                Q_EMIT fileEvent(FsCreated, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_RENAME) {
                Q_EMIT fileEvent(FsRenamed, folder, name, oldFolder, oldName, isFolder, pid);
            }
            if (meta->mask & FAN_MOVED_FROM) {
                Q_EMIT fileEvent(FsMovedAway, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_MOVED_TO) {
                Q_EMIT fileEvent(FsMovedHere, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_MODIFY) {
                Q_EMIT fileEvent(FsWritten, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_CLOSE_WRITE) {
                Q_EMIT fileEvent(FsClosedAfterWriting, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_ATTRIB) {
                Q_EMIT fileEvent(FsAttributes, folder, name, {}, {}, isFolder, pid);
            }
            if (meta->mask & FAN_DELETE) {
                Q_EMIT fileEvent(FsDeleted, folder, name, {}, {}, isFolder, pid);
            }
        }
    }
    closeAll();
}
