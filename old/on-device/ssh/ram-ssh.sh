#!/bin/sh
# Temporary SSH server on the monitor, in RAM only. NOTHING is written to the flash.
#
# Run on the MONITOR, in the recovery shell, once its Wi-Fi access point is up
# (docs/02-root-and-backup.md). The PC must be running "vimar.py serve".
#
#     sh ram-ssh.sh <PC-IP>
#
# It downloads the SSH server and your public key from the PC into /tmp/ssh, creates the device
# nodes of the flash partitions (needed for the backup) and starts the server. A reboot removes
# all of it.
PC=$1
PORT=${2:-8000}
D=/tmp/ssh

fail() { echo; echo "FAILED: $*"; exit 1; }

[ -n "$PC" ] || fail "usage: sh ram-ssh.sh <PC-IP>"
grep -q ' /tmp tmpfs ' /proc/mounts || fail "/tmp is not a RAM disk - paste the whole block of the guide first"
mkdir -p $D && cd $D || fail "cannot create $D"

echo "1/4 downloading from http://$PC:$PORT/ ..."
for f in dropbearmulti authorized_keys start.sh make-permanent.sh; do
    rm -f $f
    wget -q -O $f "http://$PC:$PORT/$f" || { rm -f $f; fail "cannot download $f - is 'vimar.py serve' running on the PC, and is the PC on the monitor's Wi-Fi?"; }
done
grep -q '^ssh-' authorized_keys || fail "authorized_keys is not an SSH public key"
chmod 755 dropbearmulti start.sh make-permanent.sh
chmod 600 authorized_keys
ln -sf dropbearmulti dropbear
ln -sf dropbearmulti dropbearkey

echo "2/4 creating the host key ..."
if [ ! -f $D/dropbear_ed25519_host_key ]; then
    ./dropbearkey -t ed25519 -f $D/dropbear_ed25519_host_key >/dev/null 2>&1 || fail "dropbearkey did not run (wrong or damaged dropbearmulti?)"
fi

echo "3/4 creating the flash device nodes (read only use, for the backup) ..."
while read -r dev rest; do
    case "$dev" in
    mtd[0-9]*:)
        n=${dev#mtd}; n=${n%:}
        [ -e "/dev/mtd$n" ] || mknod "/dev/mtd$n" c 90 $((n * 2))
        ;;
    esac
done < /proc/mtd

echo "4/4 starting the SSH server ..."
[ -f /var/run/dropbear.pid ] && kill "$(cat /var/run/dropbear.pid)" 2>/dev/null && sleep 1
sed "s#^D=.*#D=$D#" start.sh > start-ram.sh
chmod 755 start-ram.sh
sh $D/start-ram.sh
sleep 2
pidof dropbearmulti >/dev/null 2>&1 || pidof dropbear >/dev/null 2>&1 || fail "the SSH server did not start"

echo
echo "OK - temporary SSH server running (RAM only, nothing written to flash)."
echo "On the PC, stop 'vimar.py serve' with Ctrl+C, then continue with the backup."
