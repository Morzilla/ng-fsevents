#!/bin/bash
# try-here.sh: installs the service from this source tree on this machine, outside the package manager's
# folders, to try it out. As root:   sudo packaging/try-here.sh          (build first, as yourself: see below)
#                                    sudo packaging/try-here.sh remove
# It only installs what was built as the user:
#   cmake -S . -B build-local -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local \
#         -DSYSTEMD_SYSTEM_UNIT_DIR=/etc/systemd/system -DDBUS_SYSTEM_POLICY_DIR=/etc/dbus-1/system.d
#   cmake --build build-local
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
if [ "$(id -u)" != 0 ]; then echo "run it with sudo"; exit 1; fi
if [ "$1" = remove ]; then
    systemctl disable --now ng-fsevents 2>/dev/null || true
    rm -f /usr/local/libexec/ng-fseventsd /etc/systemd/system/ng-fsevents.service /etc/dbus-1/system.d/org.ng.FsEvents1.conf
    rm -rf /var/lib/ng-fsevents
    systemctl daemon-reload
    echo "removed (with its journal)"
    exit 0
fi
[ -x build-local/ng-fseventsd ] || { echo "build it first, as yourself (see the top of this file)"; exit 1; }
systemctl stop ng-fsevents 2>/dev/null || true
install -D -m 755 build-local/ng-fseventsd /usr/local/libexec/ng-fseventsd.new
mv -f /usr/local/libexec/ng-fseventsd.new /usr/local/libexec/ng-fseventsd
install -D -m 644 build-local/ng-fsevents.service /etc/systemd/system/ng-fsevents.service
install -D -m 644 data/org.ng.FsEvents1.conf /etc/dbus-1/system.d/org.ng.FsEvents1.conf
systemctl daemon-reload
systemctl reload dbus-broker 2>/dev/null || systemctl reload dbus 2>/dev/null || true
systemctl enable --now ng-fsevents
sleep 1
systemctl --no-pager --lines=5 status ng-fsevents
