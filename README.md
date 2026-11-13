# ng-fsevents

**Status: a first draft.** It builds, but it has not been run yet. Nothing here is ready to install.

A small Linux service that keeps a journal of *when something last changed under each folder* of the users'
homes, at any depth, also while no application is running. It is meant for file managers that sort folders
by date: a folder should move up when a file deep inside it is edited, which on Linux nothing tells them today.

It started from a discussion about [Dolphin NG](https://github.com/Morzilla/dolphin-ng). Dolphin NG today
finds the newest change inside a folder while it measures folder sizes, keeps it in a cache, and checks it
again when the folder is shown. That works, but it is not real time and it goes only as deep as that walk.
A KDE developer suggested the proper way: fanotify, in a root service, as a library other applications can
use. This is a sketch of that.

## Why a service, and why root

Linux has two APIs for file changes, and neither does this on its own:

- **inotify** watches one folder at a time, without recursion. Watching a whole home means one watch per folder,
  with a per-user limit.
- **fanotify** can watch a whole filesystem with one mark (`FAN_MARK_FILESYSTEM`, Linux 4.20) and, with
  `FAN_REPORT_DFID_NAME` (Linux 5.9), says in which folder and under which name each change happened. But
  filesystem marks need `CAP_SYS_ADMIN`: an application cannot ask for them.

Neither keeps a history: what happens while nobody listens is lost. macOS has FSEvents and Windows the NTFS
change journal for this; Linux has nothing alike for every filesystem.

So the listening has to happen in a small privileged service that runs all the time, and applications ask it.

## How it works

1. At start, it finds the users' homes (accounts from `UID_MIN` up) and puts one fanotify mark on each
   filesystem that holds one.
2. For each change (modified, attributes, created, deleted, moved) it turns the folder's file handle back into
   a path. A change outside every home is dropped at once and never kept, and so is one inside a hidden folder
   (caches, settings, containers): like a file manager, which does not show them. `.git` still counts.
3. It records the time on that folder and on every folder above it, up to the home.
4. The table is saved in its state directory every minute and when it stops, so it survives restarts.
5. Applications ask over D-Bus.

## D-Bus: `org.ng.FsEvents1`

On the system bus, object `/org/ng/FsEvents1`:

| Method | Answer |
|---|---|
| `Newest(s folder) → x` | when something last changed under `folder`, in seconds since the epoch (0: nothing seen since the journal began) |
| `ChangedSince(x since) → as` | the folders of your home that changed after `since`, at most 1000 |
| `Now() → x` | the service's clock, to call `ChangedSince` from later on |

```sh
gdbus call --system --dest org.ng.FsEvents1 --object-path /org/ng/FsEvents1 \
    --method org.ng.FsEvents1.Newest "$HOME/Documents"
```

## Privacy and safety

The service sees every change on the watched filesystems, so what it gives out is narrow on purpose:

- **Each caller learns about its own home only.** The caller is the uid the bus reports for the connection,
  which cannot be faked; a folder outside that home is refused.
- **No file names are given out**, only folders of the caller's home and dates.
- **Only homes are kept.** The saved table can be read by root only.
- **Few privileges.** The systemd unit keeps only `CAP_SYS_ADMIN` and `CAP_DAC_READ_SEARCH`, with no network,
  a read-only view of the system and the homes, and no new privileges.
- **Small on purpose**, a few hundred lines, so it can be read in one sitting.

## To verify before going further

- **btrfs**: a subvolume has a filesystem id of its own, and some kernels refuse filesystem marks or handle
  resolution there. Check on the kernels that matter.
- **The sandbox of the unit** against fanotify and `open_by_handle_at` (`ProtectHome=read-only`, private mounts).
- **Event load** on a busy disk, and what to do on a queue overflow (today: everything counts as changed).
- **Flatpak**: sandboxed applications cannot reach a system service like this; they would need a portal.
- Notifications pushed to the clients (today they ask), per user, never broadcast.

## Try it

```sh
cmake -B build && cmake --build build
sudo build/ng-fseventsd --print     # prints every change it sees in the homes; Ctrl+C to stop
```

Installing (`cmake --install build`) puts the service, its systemd unit and its D-Bus policy in place, but it is
too early for that.

## Licence

LGPL-2.1-or-later (see `LICENSES/`), so a library built from it can be used by any application. The build and
configuration files are CC0-1.0.
