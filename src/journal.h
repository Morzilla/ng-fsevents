// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QString>

class NewestTree;

/** What happened to a file or a folder of a home, and which program did it. */
struct FileEvent {
    enum Kind : quint32 {
        Created = 1, // it was not there before
        Modified = 2, // written to, or saved over (a program that writes a temporary file and renames it)
        Moved = 4, // renamed or moved to where it is now
        Folder = 8, // it is a folder
        Bulk = 16, // a folder that stands for many files a program changed at once (see EventJournal)
    };
    QString path;
    qint64 when = 0; // milliseconds since the epoch
    quint32 kinds = 0;
    QString program; // the name the process was started with ("firefox", "gimp-3.0"), may be empty
    QString app; // the desktop application it belongs to, when the session says so ("org.kde.kate"), may be empty
    quint32 count = 0; // Bulk: how many files
};

/**
 * The latest thing that happened to each file and folder of the users' homes: one entry per path, kept for a
 * number of days, never more than a fixed number of them.
 *
 * Left out, because nobody would look for them in a list of recent files:
 *  - what is hidden or lies inside a hidden folder, and the temporary and lock files of the programs;
 *  - a program that changes many files in a short time (a build, an archive unpacked, a synchronisation, a
 *    checkout): its files are taken back and one entry is kept for the folder they have in common.
 */
class EventJournal
{
public:
    explicit EventJournal(const NewestTree *tree);

    /** @p kinds happened to @p name in @p folder (real path) because of process @p pid. */
    void record(const QString &folder, const QString &name, quint32 kinds, int pid, qint64 when);
    /** A file with a temporary name (isNoise) was made in @p folder: if it is later renamed to the name it stands
     *  for, and nothing had that name now, that file is new (a download, a new document), not a modified one. */
    void temporaryMade(const QString &folder, const QString &name, qint64 when);
    /** @p name in @p folder is gone (a folder: with what was inside). */
    void remove(const QString &folder, const QString &name, bool isFolder);
    /** Renamed or moved by process @p pid. Either side may be outside the homes. */
    void rename(const QString &oldFolder, const QString &oldName, const QString &newFolder, const QString &newName, bool isFolder, int pid, qint64 when);

    /** The entries of @p uid's home at @p since (milliseconds) or later, newest first, at most @p limit; paths
     *  named through the home as the account has it. */
    QList<FileEvent> events(uint uid, qint64 since, int limit) const;

    /** How many entries @p uid's home has. */
    int count(uint uid) const;
    /** Drops what is older than the days kept, and the oldest beyond the number kept. */
    void expire(qint64 now);
    bool load(const QString &file);
    bool save(const QString &file) const;
    bool isDirty() const;

    /** Names no entry is kept for: hidden, temporary, lock and partial-download files. */
    static bool isNoise(const QString &name);

private:
    struct Entry {
        qint64 when = 0;
        quint32 kinds = 0;
        quint32 count = 0;
        QString program;
        QString app;
    };
    struct Who {
        QString program;
        QString app;
        qint64 asked = 0;
    };
    struct Burst {
        qint64 started = 0;
        qint64 last = 0;
        QStringList paths; // what this process had entries made for in the burst
        QString common; // Bulk: the folder that stands for them
        quint32 count = 0;
        bool bulk = false;
        QSet<size_t> counted; // Bulk: the files counted already (each once, however many events it has)
    };
    struct Temp {
        qint64 when = 0;
        QString becomes; // the name it is expected to be renamed to
        bool fresh = false; // nothing had that name when the temporary file was made
    };
    bool wanted(const QString &folder, const QString &name) const;
    Who who(int pid, qint64 when);
    void put(const QString &path, quint32 kinds, int pid, qint64 when);

    const NewestTree *m_tree;
    QHash<QString, Entry> m_entries; // real path -> the latest that happened to it
    QHash<int, Who> m_who;
    QHash<int, Burst> m_bursts;
    QHash<QString, Temp> m_temps; // temporary files being written: real path -> what is known of them
    mutable bool m_dirty = false;
};
