# ng-fsevents

**Status: an early version.** It runs as a system service and answers over D-Bus; it was tried on ext4 and on
a btrfs subvolume (Linux 7.0). No application uses it yet, and the interface may still change.

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
   filesystem that holds one, and on each filesystem mounted inside one (another disk as `~/Documents`, say).
   The marks are set again whenever the mounts change. Nothing is kept open on a filesystem, so a disk mounted
   inside a home can still be unmounted.
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
| `ChangedSince(x since) → as` | the folders of your home that changed at `since` or later, at most 1000. The second asked for is included, so a folder may come again with the next call |
| `Events(x since, u limit) → a(sxussu)` | the latest that happened to each file and folder of your home, at `since` (**milliseconds**) or later, newest first, at most `limit` (never more than 2000). Each is (path, when in milliseconds, kinds, program, application, count) |
| `Status() → a{sv}` | how the service is doing for the caller: `watched` (its home is listened to), `reason` (why not), `files` (entries of the journal of the files), `started`, `overflowed` (when events were last lost), `days`, `version` |
| `Now() → x` | the service's clock, to call `ChangedSince` from later on |

```sh
gdbus call --system --dest org.ng.FsEvents1 --object-path /org/ng/FsEvents1 \
    --method org.ng.FsEvents1.Newest "$HOME/Documents"
```

### The journal of the files

Besides the dates of the folders, the service keeps the latest thing that happened to each file and folder
of the homes, and which program did it: what a list of recent files needs to say "modified by GIMP" or
"downloaded with Firefox" for files no file manager touched. One entry per path, for 14 days, at most 20 000.

`kinds` is a sum of: 1 created, 2 modified (written to, or saved over), 4 renamed or moved to where it is,
8 it is a folder, 16 a folder that stands for many files (`count` of them) changed at once.

`program` is the name the process was started with (`firefox`, `gimp-3.0`); `application` is the desktop
application it belongs to when the session tells (`org.kde.kate`, from the name of its systemd scope; Flatpak
and Snap applications too). Either may be empty: a process that lives a moment may be gone before it is asked.

Left out, because nobody looks for them among recent files:

- what is hidden or inside a hidden folder, and temporary, lock and partial-download files;
- a file opened for writing and closed unwritten, and changes of attributes only (tags, permissions, dates);
- what is built from something else: object and bytecode files (`.o`, `.pyc`, `.class`...), and what is inside
  the folders build systems fill (`build`, `build-*`, `CMakeFiles`, `node_modules`, `__pycache__`...);
- more than 40 files changed in a row (a build, an archive unpacked, a synchronisation, a checkout): one entry
  for the folder they have in common, with their number. Counted by application; the programs that belong to
  none (compilers, shells, tools) are counted together, since a build is hundreds of short processes.

### Leaving things out

Hidden folders (`.local`, `.cache`, `.config`...) are never recorded, and neither are the disks of virtual
machines (`*.qcow2`, `*.vmdk`, `*.vdi`, `*.vhd`, `*.vhdx`). Each user can add to that in
`~/.config/ng-fsevents/exclude`, and the administrator for everyone in `/etc/ng-fsevents/exclude`; one entry
per line, read again as soon as the file is saved:

```
# files by name, with * and ? (upper and lower case alike): no entry in the journal of the files
*.iso
backup-*.tar
# a folder of the home (or an absolute path): nothing in it is recorded, as if it were hidden
KVM/
Projects/huge-dataset/
```

At most 200 entries. The user's file is read only if it is a regular file of that user, at most 64 KiB.

A file written to without pause (the disk of a running virtual machine, a database, a log) does not keep the
service busy whatever its name: past 20 writes in a second the kernel is told to keep that file's writes to
itself for a minute. Its closing is still told. Measured with 3 million writes in 8 seconds: 8 s of CPU
without this, 1 ms with it.

A program that saves by writing a temporary file and renaming it over the document counts as "modified"; as
"created" too when nothing had that name before. A file written and renamed within an instant may be taken for
a modified one: the service learns of the temporary file after the rename has happened.

## Privacy and safety

The service sees every change on the watched filesystems, so what it gives out is narrow on purpose:

- **Each caller learns about its own home only.** The caller is the uid the bus reports for the connection,
  which cannot be faked; a folder outside that home is refused. So is a folder of another user's home that
  lies inside the caller's: a folder belongs to the deepest home that holds it.
- **A caller gets its own files' names and nobody else's**: `Newest` and `ChangedSince` give folders and dates
  only, `Events` the names of the caller's files and of the programs that changed them.
- **Never contents.** fanotify is used for notifications only (which entry of which folder, which kind of
  change, which process): the service never opens a user's file, and does not ask the kernel for the mode
  that hands files over or lets it allow and deny access.
- **Only homes are kept.** The saved tables can be read by root only, and the journal of the files forgets
  what is older than 14 days.
- **Few privileges.** The systemd unit keeps only `CAP_SYS_ADMIN` and `CAP_DAC_READ_SEARCH`, with no network,
  a read-only view of the system and the homes, and no new privileges.
- **Small on purpose**, a few hundred lines, so it can be read in one sitting.

## Filesystems

- **ext4, XFS and the like**: one mark per filesystem.
- **btrfs**: the kernel refuses a filesystem mark on a subvolume (`EXDEV`), and a home is very often one. The
  service then mounts the top of that btrfs filesystem read-only in a folder of its own (`/run/ng-fsevents`,
  inside its private mount namespace: nobody else sees it), marks it there, and gives the paths as they are
  under the home. Only for the filesystem of a home, since that mount keeps the filesystem busy.
- **overlayfs** (a live system with persistence, some containers): the mark is refused, and nothing is
  recorded there. The service says so in its log and answers 0.
- **Network and FUSE filesystems**: no events from the kernel, nothing recorded.

What is kept is bounded: the journal of the files holds 14 days and at most 20 000 entries; the table of the
folders drops what nothing happened in for half a year, and the oldest beyond 200 000.

When the kernel's queue overflows, events are lost: from then on every folder counts as changed at that time,
also the ones nothing was recorded for.

A home that is a symbolic link is known by both names: ask through either, the answers use the account's.

## Still open

- **Flatpak**: sandboxed applications cannot reach a system service like this; they would need a portal.
- Notifications pushed to the clients (today they ask), per user, never broadcast.

## Try it

```sh
cmake -B build && cmake --build build
sudo build/ng-fseventsd --print     # prints every change it sees in the homes; Ctrl+C to stop
sudo build/ng-fseventsd --print --home /some/folder   # the same for one folder, to try a filesystem out
```

To install the service, configure with `-DCMAKE_INSTALL_PREFIX=/usr`, then `sudo cmake --install build` and
`sudo systemctl enable --now ng-fsevents`. The systemd unit and the D-Bus policy go where systemd and the system
bus read them (`/usr/lib/systemd/system`, `/usr/share/dbus-1/system.d`) whatever the prefix; `SYSTEMD_SYSTEM_UNIT_DIR`
and `DBUS_SYSTEM_POLICY_DIR` change that.

## Licence

LGPL-2.1-or-later (see `LICENSES/`), so a library built from it can be used by any application. The build and
configuration files are CC0-1.0.
