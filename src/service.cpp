// SPDX-License-Identifier: LGPL-2.1-or-later
#include "service.h"
#include "newesttree.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>

FsEventsService::FsEventsService(NewestTree *tree, QObject *parent)
    : QObject(parent)
    , m_tree(tree)
{
}

bool FsEventsService::callerUid(uint *uid)
{
    const auto reply = connection().interface()->serviceUid(message().service());
    if (!reply.isValid() || !m_tree->hasHome(reply.value())) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("only the users with a home can ask"));
        return false;
    }
    *uid = reply.value();
    return true;
}

qlonglong FsEventsService::Newest(const QString &folder)
{
    uint uid = 0;
    if (!callerUid(&uid)) {
        return 0;
    }
    bool allowed = false;
    const qint64 when = m_tree->newest(uid, folder, &allowed);
    if (!allowed) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("only folders of your own home"));
        return 0;
    }
    return when;
}

QStringList FsEventsService::ChangedSince(qlonglong since)
{
    uint uid = 0;
    if (!callerUid(&uid)) {
        return {};
    }
    return m_tree->changedSince(uid, since, 1000);
}

qlonglong FsEventsService::Now()
{
    return QDateTime::currentSecsSinceEpoch();
}
