#!/bin/sh
D=/mntfs/dropbear
[ -d /dev/pts ] || mkdir -p /dev/pts
mount | grep -q devpts || mount -t devpts devpts /dev/pts 2>/dev/null
[ -e /dev/ptmx ] || mknod /dev/ptmx c 5 2
chmod 666 /dev/ptmx 2>/dev/null
"$D/dropbearmulti" dropbear -E -s -p 22 -r "$D/dropbear_ed25519_host_key" -D "$D" -P /var/run/dropbear.pid &
