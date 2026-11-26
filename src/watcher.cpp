// SPDX-License-Identifier: LGPL-2.1-or-later
#include "watcher.h"

#include <QFile>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/fanotify.h>
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

FanotifyWatcher::FanotifyWatcher(QObject *parent)
    : QObject(parent)
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
    for (int fd : std::as_const(m_mountFds)) {
        ::close(fd);
    }
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

bool FanotifyWatcher::isValid() const
{
    return m_fd >= 0;
}

bool FanotifyWatcher::watch(const QString &path, QString *error)
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
    if (m_mountFds.contains(key)) {
        return true; // that filesystem is already watched
    }
    // TO VERIFY: on btrfs, a subvolume has an fsid of its own, and older kernels refuse filesystem marks there
    if (fanotify_mark(m_fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, s_mask, AT_FDCWD, local.constData()) != 0) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        return false;
    }
    const int mountFd = ::open(local.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mountFd < 0) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        return false;
    }
    m_mountFds.insert(key, mountFd);
    return true;
}

void FanotifyWatcher::readEvents()
{
    alignas(fanotify_event_metadata) char buffer[64 * 1024];
    for (;;) {
        ssize_t length = ::read(m_fd, buffer, sizeof buffer);
        if (length <= 0) {
            return; // EAGAIN: nothing more for now
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
                    const int mountFd = m_mountFds.value(fsidKey(fid->fsid), -1);
                    if (mountFd >= 0) {
                        // the folder's handle back into a path (the folder may be gone already: ESTALE)
                        const int dirFd = ::open_by_handle_at(mountFd, handle, O_PATH | O_CLOEXEC);
                        if (dirFd >= 0) {
                            char target[4096];
                            const QByteArray link = "/proc/self/fd/" + QByteArray::number(dirFd);
                            const ssize_t n = ::readlink(link.constData(), target, sizeof target - 1);
                            ::close(dirFd);
                            if (n > 0) {
                                Q_EMIT changed(QFile::decodeName(QByteArray(target, n)), QFile::decodeName(name));
                            }
                        }
                    }
                }
                record += header->len;
            }
        }
    }
}
