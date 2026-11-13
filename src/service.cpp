// SPDX-License-Identifier: LGPL-2.1-or-later
#include "service.h"
#include "newesttree.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>
#include <QDir>

FsEventsService::FsEventsService(NewestTree *tree, QObject *parent)
    : QObject(parent)
    , m_tree(tree)
{
}

QString FsEventsService::callerHome()
{
    const auto uid = connection().interface()->serviceUid(message().service());
    const QString home = uid.isValid() ? m_tree->homeOf(uid.value()) : QString();
    if (home.isEmpty()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("only the users with a home can ask"));
    }
    return home;
}

qlonglong FsEventsService::Newest(const QString &folder)
{
    const QString home = callerHome();
    if (home.isEmpty()) {
        return 0;
    }
    const QString clean = QDir::cleanPath(folder);
    if (clean != home && !clean.startsWith(home + QLatin1Char('/'))) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("only folders of your own home"));
        return 0;
    }
    return m_tree->newest(clean);
}

QStringList FsEventsService::ChangedSince(qlonglong since)
{
    const QString home = callerHome();
    if (home.isEmpty()) {
        return {};
    }
    return m_tree->changedSince(home, since, 1000);
}

qlonglong FsEventsService::Now()
{
    return QDateTime::currentSecsSinceEpoch();
}
