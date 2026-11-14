#!/bin/sh
# Make the SSH server permanent: copy it from RAM (/tmp/ssh) to the monitor's data partition and
# add ONE line to the boot script so it starts at every power-on.
#
# Run on the MONITOR after the flash backup, from the PC:
#     ssh root@192.168.111.1 "sh /tmp/ssh/make-permanent.sh"
# (or type  sh /tmp/ssh/make-permanent.sh  on the serial console).
#
# What it changes:
#   /mntfs/dropbear/     new folder on the data partition (the monitor's own writable storage)
#   /root/boot_app.sh    one added line, before "cmdapp &". The original is kept as
#                        /mntfs/dropbear/boot_app.sh.orig
# Every step is checked. If the boot script does not look exactly as expected, nothing is written.
# Safe to run twice.
#
# To take the line out again (the SSH server then no longer starts at power-on):
#     sh /mntfs/dropbear/make-permanent.sh undo
MODE=${1:-install}
S=/tmp/ssh
P=/mntfs/dropbear
B=/root/boot_app.sh
HOOK='[ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &'

fail() { echo; echo "FAILED: $*"; echo "Nothing else was changed. See 'If something goes wrong' in docs/02-root-and-backup.md."; exit 1; }
md5() { md5sum < "$1" | cut -c1-32; }

case "$MODE" in install|undo) ;; *) fail "usage: sh make-permanent.sh [undo]" ;; esac

if [ "$MODE" = install ]; then
    for f in dropbearmulti authorized_keys dropbear_ed25519_host_key start.sh; do
        [ -f $S/$f ] || fail "$S/$f is missing - run ram-ssh.sh first (docs/02-root-and-backup.md, step 3)"
    done
    grep -q '^ssh-' $S/authorized_keys || fail "$S/authorized_keys is not an SSH public key"
fi

# ---- 1. data partition ---------------------------------------------------------------------
echo "1/3 data partition (/mntfs) ..."
if ! grep -q ' /mntfs ' /proc/mounts; then
    grep -q '^mtd4:.*"data"' /proc/mtd || fail "mtd4 is not the 'data' partition on this monitor (cat /proc/mtd)"
    if [ ! -d /sys/class/ubi/ubi4 ]; then
        if [ ! -e /dev/ubi_ctrl ]; then
            maj=10; min=61
            [ -f /sys/class/misc/ubi_ctrl/dev ] && IFS=: read -r maj min < /sys/class/misc/ubi_ctrl/dev
            mknod /dev/ubi_ctrl c "$maj" "$min" || fail "cannot create /dev/ubi_ctrl"
        fi
        /usr/local/bin/ubiattach -m 4 -d 4 /dev/ubi_ctrl || fail "ubiattach failed"
    fi
    [ -f /sys/class/ubi/ubi4_0/dev ] || fail "the data volume ubi4_0 did not appear"
    if [ ! -e /dev/ubi4_0 ]; then
        IFS=: read -r maj min < /sys/class/ubi/ubi4_0/dev
        mknod /dev/ubi4_0 c "$maj" "$min" || fail "cannot create /dev/ubi4_0"
    fi
    mount -t ubifs ubi4_0 /mntfs || fail "cannot mount the data partition"
fi
[ -d /mntfs/wifi ] || fail "/mntfs does not look like the monitor's data partition (no wifi folder)"

# ---- undo: put the original boot script back ------------------------------------------------
if [ "$MODE" = undo ]; then
    echo "2/3 (nothing to copy)"
    echo "3/3 boot script ($B) ..."
    [ -f $P/boot_app.sh.orig ] || fail "$P/boot_app.sh.orig (the saved original) does not exist"
    if [ "$(md5 $B)" = "$(md5 $P/boot_app.sh.orig)" ]; then
        echo "    the boot script is already the original - nothing to write"
    else
        grep -v -x -F "$HOOK" $B > /tmp/boot_app.chk
        [ "$(md5 /tmp/boot_app.chk)" = "$(md5 $P/boot_app.sh.orig)" ] \
            || fail "$B differs from the saved original by more than the one SSH line - not touched"
        mount -o remount,rw / || fail "cannot make the root filesystem writable - not touched"
        cp $P/boot_app.sh.orig $B
        sync
        mount -o remount,ro / || echo "    note: could not switch the root filesystem back to read-only (harmless, a reboot does it)"
        [ "$(md5 $B)" = "$(md5 $P/boot_app.sh.orig)" ] || fail "the boot script is not the original after writing"
        echo "    original boot script restored"
    fi
    rm -f /tmp/boot_app.chk
    echo
    echo "OK - the monitor's firmware is as delivered again; the SSH server no longer starts at power-on."
    echo "(The files in /mntfs/dropbear and /mntfs/vimarbridge are still there but are not used.)"
    exit 0
fi

# ---- 2. copy the SSH server ----------------------------------------------------------------
echo "2/3 copying the SSH server to $P ..."
mkdir -p $P || fail "cannot create $P"
cp $S/dropbearmulti $P/dropbearmulti.new && chmod 755 $P/dropbearmulti.new && mv $P/dropbearmulti.new $P/dropbearmulti \
    || fail "cannot copy dropbearmulti"
[ "$(md5 $S/dropbearmulti)" = "$(md5 $P/dropbearmulti)" ] || fail "dropbearmulti was not copied correctly"
cp $S/authorized_keys $P/authorized_keys && chmod 600 $P/authorized_keys || fail "cannot copy authorized_keys"
if [ ! -f $P/dropbear_ed25519_host_key ]; then
    cp $S/dropbear_ed25519_host_key $P/dropbear_ed25519_host_key || fail "cannot copy the host key"
fi
chmod 600 $P/dropbear_ed25519_host_key
ln -sf dropbearmulti $P/dropbear
ln -sf dropbearmulti $P/dropbearkey
if [ ! -f $P/start.sh ]; then                       # an existing start.sh is kept (it may start more than SSH)
    cp $S/start.sh $P/start.sh || fail "cannot copy start.sh"
fi
chmod 755 $P/start.sh
[ "$0" = $P/make-permanent.sh ] || cp "$0" $P/make-permanent.sh    # kept on the monitor for a later "undo"
sync
$P/dropbearmulti dropbearkey -y -f $P/dropbear_ed25519_host_key >/dev/null 2>&1 || fail "the copied SSH server does not run"

# ---- 3. one line in the boot script --------------------------------------------------------
echo "3/3 boot script ($B) ..."
if grep -q -x -F "$HOOK" $B; then
    echo "    the line is already there - nothing to write"
else
    [ "$(grep -c '^cmdapp &' $B)" = 1 ] || fail "$B is not the expected boot script (no single 'cmdapp &' line) - not touched"
    [ -f $P/boot_app.sh.orig ] || cp $B $P/boot_app.sh.orig || fail "cannot save the original boot script"
    [ "$(md5 $B)" = "$(md5 $P/boot_app.sh.orig)" ] || fail "$P/boot_app.sh.orig differs from $B - not touched"
    sync

    # dry run in RAM: the result must be the original plus exactly this one line, right before "cmdapp &"
    sed '/^cmdapp &/i [ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &' $B > /tmp/boot_app.new
    grep -v -x -F "$HOOK" /tmp/boot_app.new > /tmp/boot_app.chk
    [ "$(md5 /tmp/boot_app.chk)" = "$(md5 $B)" ] || fail "dry run: unexpected result - not touched"
    [ "$(grep -c -x -F "$HOOK" /tmp/boot_app.new)" = 1 ] || fail "dry run: the line was not added exactly once - not touched"
    [ "$(grep -n -x -F "$HOOK" /tmp/boot_app.new | cut -d: -f1)" = "$(grep -n '^cmdapp &' $B | cut -d: -f1)" ] \
        || fail "dry run: the line is not right before 'cmdapp &' - not touched"

    # the one flash write
    mount -o remount,rw / || fail "cannot make the root filesystem writable - not touched"
    sed -i '/^cmdapp &/i [ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &' $B
    sync
    mount -o remount,ro / || echo "    note: could not switch the root filesystem back to read-only (harmless, a reboot does it)"

    now=$(md5 $B)
    if [ "$now" = "$(md5 $P/boot_app.sh.orig)" ]; then
        fail "the boot script could not be written - it is unchanged"
    elif [ "$now" != "$(md5 /tmp/boot_app.new)" ]; then
        echo "    the written file is not what was expected: restoring the original"
        mount -o remount,rw /
        cp $P/boot_app.sh.orig $B
        sync
        mount -o remount,ro /
        [ "$(md5 $B)" = "$(md5 $P/boot_app.sh.orig)" ] && fail "boot script restored to the original (no change made)"
        fail "could NOT restore the boot script - do not power off; follow 'If something goes wrong'"
    fi
    echo "    added before line $(grep -n '^cmdapp &' $B | cut -d: -f1):  $HOOK"
fi
rm -f /tmp/boot_app.new /tmp/boot_app.chk

echo
echo "OK - the SSH server is installed and will start at every power-on."
echo "Now: switch the monitor off and on (do not touch the keyboard), wait 1-2 minutes,"
echo "find its address in your router and log in:   ssh root@<MONITOR_IP>"
