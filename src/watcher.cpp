// SPDX-License-Identifier: LGPL-2.1-or-later
#include "watcher.h"

#include <QDir>
#include <QFile>
#include <QSocketNotifier>

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

// what changes the date of a folder's contents
constexpr uint64_t s_mask = FAN_MODIFY | FAN_ATTRIB | FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_ONDIR;
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
    if (fanotify_mark(m_fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, s_mask, AT_FDCWD, local.constData()) != 0) {
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
    if (fanotify_mark(m_fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, s_mask, AT_FDCWD, localTop.constData()) != 0 || ::statfs(localTop.constData(), &sfs) != 0) {
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
            // the information records follow the metadata
            const char *record = reinterpret_cast<const char *>(meta) + meta->metadata_len;
            const char *end = reinterpret_cast<const char *>(meta) + meta->event_len;
            while (record + sizeof(fanotify_event_info_header) <= end) {
                const auto *header = reinterpret_cast<const fanotify_event_info_header *>(record);
                if (header->len == 0) {
                    break;
                }
                if (header->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    const auto *fid = reinterpret_cast<const fanotify_event_info_fid *>(record);
                    auto *handle = reinterpret_cast<file_handle *>(const_cast<unsigned char *>(fid->handle));
                    const char *name = reinterpret_cast<const char *>(handle->f_handle + handle->handle_bytes);
                    const int mountFd = rootFd(fsidKey(fid->fsid));
                    if (mountFd >= 0) {
                        // the folder's handle back into a path (the folder may be gone already: ESTALE)
                        const int dirFd = ::open_by_handle_at(mountFd, handle, O_PATH | O_CLOEXEC);
                        if (dirFd >= 0) {
                            char target[4096];
                            const QByteArray link = "/proc/self/fd/" + QByteArray::number(dirFd);
                            const ssize_t n = ::readlink(link.constData(), target, sizeof target - 1);
                            ::close(dirFd);
                            if (n > 0) {
                                Q_EMIT changed(translated(QFile::decodeName(QByteArray(target, n))), QFile::decodeName(name));
                            }
                        }
                    }
                }
                record += header->len;
            }
        }
    }
    closeAll();
}
