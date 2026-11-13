// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ng-fseventsd: a journal of when things changed in each folder of the users' homes, from fanotify.
//
//   ng-fseventsd            the service, on the system bus (org.ng.FsEvents1), state in $STATE_DIRECTORY
//   ng-fseventsd --print    no bus and no state: print every change it sees in the homes (to try it out)

#include "newesttree.h"
#include "service.h"
#include "watcher.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDateTime>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include <csignal>

namespace
{
void quitOnSignal(int)
{
    QCoreApplication::quit();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ng-fseventsd"));
    const bool print = app.arguments().contains(QStringLiteral("--print"));
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QList<NewestTree::Home> homes = NewestTree::userHomes();
    if (homes.isEmpty()) {
        err << "no user homes found\n";
        return 1;
    }
    NewestTree tree(homes);

    FanotifyWatcher watcher;
    if (!watcher.isValid()) {
        err << "fanotify is not available (it needs root and Linux 5.9 or newer)\n";
        return 1;
    }
    int watched = 0;
    for (const NewestTree::Home &home : homes) {
        QString error;
        if (watcher.watch(home.path, &error)) {
            ++watched;
        } else {
            err << "cannot watch the filesystem of " << home.path << ": " << error << "\n";
        }
    }
    if (watched == 0) {
        return 1;
    }

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

    std::signal(SIGTERM, quitOnSignal);
    std::signal(SIGINT, quitOnSignal);

    if (print) {
        out << "watching " << watched << " filesystem(s); Ctrl+C to stop" << Qt::endl;
        return app.exec();
    }

    // the journal survives restarts: systemd gives the service a state directory of its own
    const QString state = qEnvironmentVariable("STATE_DIRECTORY", QStringLiteral("/var/lib/ng-fsevents")) + QStringLiteral("/newest.db");
    tree.load(state);
    QTimer saver;
    QObject::connect(&saver, &QTimer::timeout, &app, [&] {
        if (tree.isDirty()) {
            tree.save(state);
        }
    });
    saver.start(60 * 1000);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        tree.save(state);
    });

    FsEventsService service(&tree);
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(QStringLiteral("/org/ng/FsEvents1"), &service, QDBusConnection::ExportAllSlots)
        || !bus.registerService(QStringLiteral("org.ng.FsEvents1"))) {
        err << "cannot register org.ng.FsEvents1 on the system bus: " << bus.lastError().message() << "\n";
        return 1;
    }
    return app.exec();
}
