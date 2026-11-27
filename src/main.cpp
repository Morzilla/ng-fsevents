// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ng-fseventsd: a journal of when things changed in each folder of the users' homes, from fanotify.
//
//   ng-fseventsd            the service, on the system bus (org.ng.FsEvents1), state in $STATE_DIRECTORY
//   ng-fseventsd --print    no bus and no state: print every change it sees in the homes (to try it out)

#include "journal.h"
#include "newesttree.h"
#include "service.h"
#include "watcher.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSocketNotifier>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>

namespace
{
bool hiddenBelow(const QString &path, const QString &root)
{
    const auto parts = QStringView(path).mid(root.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QStringView part : parts) {
        if (part.startsWith(QLatin1Char('.'))) {
            return true;
        }
    }
    return false;
}
}

int main(int argc, char **argv)
{
    // SIGTERM and SIGINT are read from a descriptor in the event loop (nothing of Qt may be called from a signal
    // handler): blocked here, before any thread exists
    sigset_t quitSignals;
    sigemptyset(&quitSignals);
    sigaddset(&quitSignals, SIGTERM);
    sigaddset(&quitSignals, SIGINT);
    sigprocmask(SIG_BLOCK, &quitSignals, nullptr);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ng-fseventsd"));
    const bool print = app.arguments().contains(QStringLiteral("--print"));
    QTextStream out(stdout);
    QTextStream err(stderr);

    QList<NewestTree::Home> homes = NewestTree::userHomes();
    // --print --home <folder>: that folder is watched as though it were a home (to try a filesystem out)
    if (const int at = app.arguments().indexOf(QStringLiteral("--home")); print && at > 0 && at + 1 < app.arguments().size()) {
        const QString real = QFileInfo(app.arguments().at(at + 1)).canonicalFilePath();
        homes = {{0, real, real}};
    }
    if (homes.isEmpty()) {
        err << "no user homes found\n";
        return 1;
    }
    NewestTree tree(homes);

    // (systemd gives the service a folder under /run; --print as root by hand gets one of its own)
    QString privateDir = qEnvironmentVariable("RUNTIME_DIRECTORY").section(QLatin1Char(':'), 0, 0);
    struct OwnDir {
        QString path;
        ~OwnDir()
        {
            if (!path.isEmpty()) {
                QDir().rmdir(path);
            }
        }
    } ownDir; // (declared before the watcher: removed after the watcher has unmounted what it mounted there)
    if (privateDir.isEmpty()) {
        privateDir = QStringLiteral("/run/ng-fsevents-%1").arg(QCoreApplication::applicationPid());
        if (QDir().mkpath(privateDir)) {
            ownDir.path = privateDir;
        }
    }
    FanotifyWatcher watcher(privateDir);
    if (!watcher.isValid()) {
        err << "fanotify is not available (it needs root and Linux 5.9 or newer)\n";
        return 1;
    }
    // one mark per filesystem: the one of each home, and every one mounted inside a home (another disk as
    // ~/Documents, say). Again whenever the mounts change. Not inside a hidden folder: nothing there is kept
    QHash<QString, QString> refused; // the root that could not be marked -> why
    const auto markAll = [&] {
        int watched = 0;
        const QList<MountEntry> table = mountTable();
        for (const NewestTree::Home &home : homes) {
            QStringList roots{home.real};
            for (const MountEntry &entry : table) {
                if (entry.point.startsWith(home.real + QLatin1Char('/')) && !hiddenBelow(entry.point, home.real) && !roots.contains(entry.point)) {
                    roots.append(entry.point);
                }
            }
            for (const QString &root : std::as_const(roots)) {
                QString error;
                // (the private mount of a btrfs top only for the home's own filesystem: it keeps it busy)
                if (watcher.watch(root, &error, root == home.real)) {
                    ++watched;
                    refused.remove(root);
                } else if (!refused.contains(root)) {
                    refused.insert(root, error); // said once
                    err << "cannot watch the filesystem of " << root << ": " << error << Qt::endl;
                }
            }
        }
        return watched;
    };
    const int watched = markAll();
    if (watched == 0) {
        return 1;
    }
    // the kernel flags the mount table as "exceptional" when it changes
    QFile mounts(QStringLiteral("/proc/self/mountinfo"));
    QTimer remark;
    remark.setSingleShot(true);
    remark.setInterval(500);
    QObject::connect(&remark, &QTimer::timeout, &app, markAll);
    QSocketNotifier *mountsChanged = nullptr;
    if (mounts.open(QIODevice::ReadOnly)) {
        mountsChanged = new QSocketNotifier(mounts.handle(), QSocketNotifier::Exception, &app);
        QObject::connect(mountsChanged, &QSocketNotifier::activated, &app, [&] {
            mounts.seek(0);
            mounts.readAll(); // (read again: that is what arms the next notice)
            remark.start();
        });
    }

    // what happened to each file, and by which program
    EventJournal journal(&tree);
    QSet<QString> written; // written to since they were last closed (a file closed unwritten was not modified)
    QObject::connect(&watcher, &FanotifyWatcher::fileEvent, &app,
                     [&](int what, const QString &folder, const QString &name, const QString &oldFolder, const QString &oldName, bool isFolder, int pid) {
        if (pid == QCoreApplication::applicationPid()) {
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const quint32 folderBit = isFolder ? quint32(FileEvent::Folder) : 0;
        switch (what) {
        case FsCreated:
            if (!isFolder && EventJournal::isNoise(name)) {
                journal.temporaryMade(folder, name, now);
            }
            journal.record(folder, name, FileEvent::Created | folderBit, pid, now);
            break;
        case FsWritten:
            if (!isFolder && !EventJournal::isNoise(name)) {
                if (written.size() > 20000) {
                    written.clear();
                }
                written.insert(folder + QLatin1Char('/') + name);
            }
            break;
        case FsClosedAfterWriting:
            if (written.remove(folder + QLatin1Char('/') + name)) {
                journal.record(folder, name, FileEvent::Modified, pid, now);
            }
            break;
        case FsRenamed:
            journal.rename(oldFolder, oldName, folder, name, isFolder, pid, now);
            break;
        case FsMovedHere:
            journal.record(folder, name, FileEvent::Moved | folderBit, pid, now);
            break;
        case FsMovedAway:
        case FsDeleted:
            journal.remove(folder, name, isFolder);
            break;
        default: // (attributes: tags, permissions, dates; not something that "happened to the file")
            break;
        }
    });

    // one write is often several events: --print shows each folder at most once a second
    qint64 printedSecond = 0;
    QSet<QString> printed;
    QObject::connect(&watcher, &FanotifyWatcher::changed, &app, [&](const QString &folder, const QString &name) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const bool kept = tree.touch(folder, now);
        if (print && kept) {
            if (now != printedSecond) {
                printedSecond = now;
                printed.clear();
            }
            if (!printed.contains(folder)) {
                printed.insert(folder);
                out << QDateTime::fromSecsSinceEpoch(now).toString(Qt::ISODate) << "  " << folder << "  " << name << Qt::endl;
            }
        }
    });
    QObject::connect(&watcher, &FanotifyWatcher::overflowed, &app, [&] {
        tree.touchAll(QDateTime::currentSecsSinceEpoch());
        err << "event queue overflowed: everything counts as changed\n";
    });

    const int signalFd = signalfd(-1, &quitSignals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalFd < 0) {
        err << "cannot listen for SIGTERM and SIGINT\n";
        return 1;
    }
    QSocketNotifier quitNotifier(signalFd, QSocketNotifier::Read);
    QObject::connect(&quitNotifier, &QSocketNotifier::activated, &app, [&] {
        signalfd_siginfo info;
        if (::read(signalFd, &info, sizeof info) == sizeof info) {
            QCoreApplication::quit();
        }
    });

    if (print) {
        out << "watching " << watched << " folder tree(s); Ctrl+C to stop" << Qt::endl;
        // the journal of the files, as the applications get it: what is new in it, once a second
        qint64 shownUntil = QDateTime::currentMSecsSinceEpoch();
        QTimer shower;
        QObject::connect(&shower, &QTimer::timeout, &app, [&] {
            QList<FileEvent> fresh;
            QSet<uint> asked;
            for (const NewestTree::Home &home : homes) {
                if (!asked.contains(home.uid)) {
                    asked.insert(home.uid);
                    fresh += journal.events(home.uid, shownUntil + 1, 200);
                }
            }
            for (auto it = fresh.crbegin(); it != fresh.crend(); ++it) {
                shownUntil = qMax(shownUntil, it->when);
                QStringList kinds;
                kinds += (it->kinds & FileEvent::Created) ? QStringLiteral("created") : QString();
                kinds += (it->kinds & FileEvent::Modified) ? QStringLiteral("modified") : QString();
                kinds += (it->kinds & FileEvent::Moved) ? QStringLiteral("moved") : QString();
                kinds += (it->kinds & FileEvent::Folder) ? QStringLiteral("folder") : QString();
                kinds += (it->kinds & FileEvent::Bulk) ? QStringLiteral("bulk:%1").arg(it->count) : QString();
                kinds.removeAll(QString());
                out << "    file: " << it->path << "  [" << kinds.join(QLatin1Char(',')) << "]  by " << (it->program.isEmpty() ? QStringLiteral("?") : it->program)
                    << (it->app.isEmpty() ? QString() : QStringLiteral(" (") + it->app + QLatin1Char(')')) << Qt::endl;
            }
        });
        shower.start(1000);
        return app.exec();
    }

    // the journal survives restarts: systemd gives the service a state directory of its own
    const QString stateDir = qEnvironmentVariable("STATE_DIRECTORY", QStringLiteral("/var/lib/ng-fsevents")).section(QLatin1Char(':'), 0, 0);
    const QString state = stateDir + QStringLiteral("/newest.db");
    const QString events = stateDir + QStringLiteral("/events.db");
    tree.load(state);
    journal.load(events);
    journal.expire(QDateTime::currentMSecsSinceEpoch());
    tree.expire(QDateTime::currentSecsSinceEpoch());
    QTimer saver;
    int saves = 0;
    QObject::connect(&saver, &QTimer::timeout, &app, [&] {
        if (tree.isDirty()) {
            tree.save(state);
        }
        if (++saves % 60 == 0) { // (hourly)
            journal.expire(QDateTime::currentMSecsSinceEpoch());
            tree.expire(QDateTime::currentSecsSinceEpoch());
        }
        if (journal.isDirty()) {
            journal.save(events);
        }
    });
    saver.start(60 * 1000);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        tree.save(state);
        journal.save(events);
    });

    FsEventsService service(&tree, &journal, [&refused](const QString &home) {
        return home.isEmpty() ? QStringLiteral("no home") : refused.value(home);
    });
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(QStringLiteral("/org/ng/FsEvents1"), &service, QDBusConnection::ExportAllSlots)
        || !bus.registerService(QStringLiteral("org.ng.FsEvents1"))) {
        err << "cannot register org.ng.FsEvents1 on the system bus: " << bus.lastError().message() << "\n";
        return 1;
    }
    return app.exec();
}
