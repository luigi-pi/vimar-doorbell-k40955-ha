#!/bin/sh
# B1 RAM test (T52): run mainapp with a patched base.lua, bind-mounted over the read-only original.
# Nothing is written to flash or /mntfs. UNDO = reboot (the bind mount and the restarted mainapp are gone).
# usage: sh /tmp/vb/lua_b1.sh            (expects /tmp/vb/lua/base.lua from make_patch.py)
# The bridge keeps running; only mainapp is restarted (it reads the Lua files at start).
O=/usr/local/app/luaprj/src/base/base.lua
N=/tmp/vb/lua/base.lua
ORIG=77d3402119e91d34042874b2a776fc93
PATCHED=88d80b8b01aea3c9d2f1eb3f55c1c627
grep -q " $O " /proc/mounts && { echo "already bind-mounted (reboot to undo)"; exit 1; }
[ "$(md5sum $O | cut -c1-32)" = $ORIG ] || { echo "original base.lua differs from the analysed one, abort"; exit 1; }
[ "$(md5sum $N | cut -c1-32)" = $PATCHED ] || { echo "$N is not the expected patched file, abort"; exit 1; }
pidof vimarbridge >/dev/null && [ -L /dev/ttySGK1 ] || { echo "bridge not active, abort"; exit 1; }
mount --bind $N $O || mount -o bind $N $O || { echo "bind mount failed, nothing changed"; exit 1; }
echo "--- $O now: $(md5sum $O | cut -c1-32)"

P=/mntfs/vimarbridge
APP=/usr/local/app/mainapp; INI=/usr/local/config/goke_prm.ini
if [ -x $P/app/mainapp ] && [ ! -e $P/disable-app ]; then
    APP=$P/app/mainapp; [ -f $P/app/goke_prm.ini ] && INI=$P/app/goke_prm.ini
fi
kill $(pidof interDebug) $(pidof mainapp) 2>/dev/null
i=0; while pidof mainapp >/dev/null && [ $i -lt 20 ]; do usleep 100000; i=$((i+1)); done
pidof mainapp >/dev/null && kill -9 $(pidof mainapp)
cd /usr/local/app
setsid env -i \
    TSLIB_ROOT=/usr/local TSLIB_TSDEVICE=/dev/input/event0 USER=root \
    LD_LIBRARY_PATH=/lib:/usr/lib GOKE_CONFIG_FILE=$INI \
    HOME=/ TSLIB_FBDEVICE=/dev/fb0 TSLIB_CONSOLEDEVICE=none TERM=vt102 \
    PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin:/usr/local/app/:/usr/local/bin/wifi/ \
    TSLIB_CONFFILE=/usr/local/app/ts.config SHELL=/bin/sh PWD=/usr/local/app \
    interDebug $APP 9527 </dev/console >/dev/console 2>&1 &
sleep 12
if pidof mainapp >/dev/null; then
    echo "--- mainapp running: $(readlink /proc/$(pidof mainapp)/exe)"
    ls -l /proc/$(pidof mainapp)/fd 2>/dev/null | grep -q pts && echo "--- mainapp on the bridge pty: OK" || echo "--- WARNING: mainapp not on the pty"
    echo "--- check the monitor screen (main page must appear). Undo = reboot."
else
    echo "--- mainapp NOT running: reboot now (ssh vimar reboot) to restore stock"
fi
