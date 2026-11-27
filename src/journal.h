// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class NewestTree;

/** What happened to a file or a folder of a home, and which program did it. */
struct FileEvent {
    enum Kind : quint32 {
        Created = 1, // it was not there before
        Modified = 2, // written to, or saved over (a program that writes a temporary file and renames it)
        Moved = 4, // renamed or moved to where it is now
        Folder = 8, // it is a folder
        Automatic = 32, // done by something that works on its own (a synchronisation, an updater, a timer, the
                        // desktop's own services), not by an application or a terminal the user was at
    };
    QString path;
    qint64 when = 0; // milliseconds since the epoch
    quint32 kinds = 0;
    QString program; // the name the process was started with ("firefox", "gimp-3.0"), may be empty
    QString app; // the desktop application it belongs to, when the session says so ("org.kde.kate"), may be empty
    quint64 operation = 0; // the operation it was part of: see FileOperation
};

/**
 * Things done in one go: the files one application (or the tools at work in a home: a build is hundreds of
 * short processes) changed one after the other, each within seconds of the one before. An archive unpacked, a
 * build, a synchronisation, a checkout; also two files saved in a row. What to make of it is the reader's
 * choice: a list of recent files shows a few files as they are and many as one line that opens.
 */
struct FileOperation {
    quint64 id = 0;
    QString folder; // the folder its files have in common
    qint64 first = 0; // milliseconds since the epoch
    qint64 last = 0;
    quint32 count = 0; // how many files, also the ones no entry is kept for (see EventJournal)
    quint32 kinds = 0; // of FileEvent::Kind, summed
    QString program;
    QString app;
};

/**
 * The latest thing that happened to each file and folder of the users' homes: one entry per path, kept for a
 * number of days, never more than a fixed number of them. Each entry says which operation it was part of.
 *
 * Left out, because nobody would look for them in a list of recent files:
 *  - what is hidden or lies inside a hidden folder, and the temporary and lock files of the programs;
 *  - what is built from something else: object and bytecode files, what is inside the folders of build systems;
 *  - what the user or the administrator excluded;
 *  - past a number of files, the rest of one operation: they are counted, the first ones have their entries.
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
    /** The exclusions changed: the entries they now cover go. */
    void prune();

    /** The entries of @p uid's home at @p since (milliseconds) or later, newest first, at most @p limit; paths
     *  named through the home as the account has it. */
    QList<FileEvent> events(uint uid, qint64 since, int limit) const;
    /** The operations of @p uid's home that ended at @p since or later and changed at least @p atLeast files,
     *  newest first, at most @p limit. */
    QList<FileOperation> operations(uint uid, qint64 since, int atLeast, int limit) const;
    /** How many entries @p uid's home has. */
    int count(uint uid) const;

    /** Drops what is older than the days kept, and the oldest beyond the number kept. */
    void expire(qint64 now);
    bool load(const QString &file);
    bool save(const QString &file) const;
    bool isDirty() const;

    /** Names no entry is kept for: hidden, temporary, lock and partial-download files, what is built. */
    static bool isNoise(const QString &name);

private:
    struct Entry {
        qint64 when = 0;
        quint32 kinds = 0;
        quint64 operation = 0;
        QString program;
        QString app;
    };
    struct Who {
        QString program;
        QString app;
        QString service; // the systemd service it runs in when it works on its own, or empty
        qint64 asked = 0;
    };
    struct Operation {
        QString home; // real path
        QString folder; // real path
        qint64 first = 0;
        qint64 last = 0;
        quint32 count = 0;
        quint32 kinds = 0;
        QString program;
        QString app;
    };
    struct Live {
        quint64 id = 0;
        QSet<size_t> counted; // the files counted already (each once, however many events it has)
    };
    struct Temp {
        qint64 when = 0;
        QString becomes; // the name it is expected to be renamed to
        bool fresh = false; // nothing had that name when the temporary file was made
    };
    bool wanted(const QString &folder, const QString &name, bool isFolder) const;
    Who who(int pid, qint64 when);
    void put(const QString &path, quint32 kinds, int pid, qint64 when);
    quint64 operationFor(const Who &by, const QString &path, bool isFolder, qint64 when, quint32 kinds, bool *keepEntry);

    const NewestTree *m_tree;
    QHash<QString, Entry> m_entries; // real path -> the latest that happened to it
    QHash<quint64, Operation> m_operations;
    QHash<QString, Live> m_live; // who is at work (an application, or the tools of a home) -> its operation in course
    quint64 m_nextOperation = 1;
    QHash<int, Who> m_who;
    QHash<QString, Temp> m_temps; // temporary files being written: real path -> what is known of them
    mutable bool m_dirty = false;
};
