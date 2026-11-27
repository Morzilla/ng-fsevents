// SPDX-License-Identifier: LGPL-2.1-or-later
#include "service.h"
#include "newesttree.h"

#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDateTime>

Q_DECLARE_METATYPE(FileEvent)

QDBusArgument &operator<<(QDBusArgument &argument, const FileEvent &event)
{
    argument.beginStructure();
    argument << event.path << qlonglong(event.when) << event.kinds << event.program << event.app << event.count;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, FileEvent &event)
{
    qlonglong when = 0;
    argument.beginStructure();
    argument >> event.path >> when >> event.kinds >> event.program >> event.app >> event.count;
    argument.endStructure();
    event.when = when;
    return argument;
}

FsEventsService::FsEventsService(NewestTree *tree, EventJournal *journal, std::function<QString(const QString &)> whyNotWatched, QObject *parent)
    : QObject(parent)
    , m_tree(tree)
    , m_journal(journal)
    , m_whyNotWatched(std::move(whyNotWatched))
    , m_started(QDateTime::currentSecsSinceEpoch())
{
    qDBusRegisterMetaType<FileEvent>();
    qDBusRegisterMetaType<QList<FileEvent>>();
}

QList<FileEvent> FsEventsService::Events(qlonglong since, uint limit)
{
    uint uid = 0;
    if (!callerUid(&uid)) {
        return {};
    }
    return m_journal->events(uid, since, int(qBound(1u, limit, 2000u)));
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

QVariantMap FsEventsService::Status()
{
    uint uid = 0;
    if (!callerUid(&uid)) {
        return {};
    }
    const QString reason = m_whyNotWatched(m_tree->realHomeOf(uid));
    return {{QStringLiteral("watched"), reason.isEmpty()},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("files"), uint(m_journal->count(uid))},
            {QStringLiteral("started"), qlonglong(m_started)},
            {QStringLiteral("overflowed"), qlonglong(m_tree->overflowedAt())},
            {QStringLiteral("days"), uint(14)},
            {QStringLiteral("version"), QStringLiteral("0.2")}};
}

qlonglong FsEventsService::Now()
{
    return QDateTime::currentSecsSinceEpoch();
}
