#!/bin/sh
# vimarbridge boot hook — lives on the data partition (/mntfs/vimarbridge), started from
# /mntfs/dropbear/start.sh AFTER dropbear. The rootfs is not modified.
#   disable at next boot:  touch /mntfs/vimarbridge/disable
#   log (RAM):             /tmp/vb/boot.log, bridge log /tmp/vb/bridge.log, media log /tmp/vb/audio.log
# Optional media helper: started only if /mntfs/vimarbridge/vimaraudio and media.conf (ha_ip=...) exist.
#   disable the helper only: touch /mntfs/vimarbridge/disable-audio
# Optional Lua patch (B1, T52/T54): /mntfs/vimarbridge/lua/base.lua is bind-mounted over the read-only
#   luaprj/src/base/base.lua before mainapp is restarted behind the bridge (md5-checked, RAM mount only).
#   disable the patch only: touch /mntfs/vimarbridge/disable-lua
# Method = the proven manual swap (swap_vb.sh), done once mainapp is fully up. If the bridge
# ever dies, the original UART is restored and mainapp restarted stock (intercom keeps working).
D=/mntfs/vimarbridge
R=/tmp/vb
[ -e $D/disable ] && exit 0
mkdir -p $R
exec >>"$R/boot.log" 2>&1
bootlog() {
    level=$1; shift
    printf '%s\n' "$*"
    [ -x "$D/vimarlog" ] && "$D/vimarlog" supervisor "$level" "$*" >/dev/null 2>&1
    return 0
}
bootlog INFO "$(date) boot.sh start"
for f in vimarbridge bridge.conf broker.pem; do [ -e $D/$f ] || { bootlog ERROR "missing $D/$f, abort"; exit 1; }; done
if pidof vimarbridge >/dev/null || [ -e /dev/ttySGK1.real ]; then bootlog WARNING "bridge already active, abort"; exit 1; fi

# optional patched mainapp (T33 key lock) in $D/app: mainapp [+ goke_prm.ini]; disable: touch $D/disable-app
# optional HQ video build (key lock + 896x576 live view) in $D/app-hq/mainapp, used when $D/app.mode says "hq".
#   Switched from HA (switch "High quality video" -> bridge -> $R/app.want, see the supervisor below).
STOCK=/usr/local/app/mainapp; STOCK_INI=/usr/local/config/goke_prm.ini
KEY=$STOCK; KEY_INI=$STOCK_INI
if [ -x $D/app/mainapp ] && [ ! -e $D/disable-app ]; then
    KEY=$D/app/mainapp; [ -f $D/app/goke_prm.ini ] && KEY_INI=$D/app/goke_prm.ini
fi
HQ=$D/app-hq/mainapp
pick_app() {                   # sets APP / INI from $D/app.mode
    APP=$KEY; INI=$KEY_INI
    if [ "$(cat $D/app.mode 2>/dev/null)" = hq ] && [ -x $HQ ] && [ ! -e $D/disable-app ]; then
        APP=$HQ; INI=$STOCK_INI
    fi
}
pick_app
start_mainapp() {
    cd /usr/local/app
    setsid env -i \
        TSLIB_ROOT=/usr/local TSLIB_TSDEVICE=/dev/input/event0 USER=root \
        LD_LIBRARY_PATH=/lib:/usr/lib GOKE_CONFIG_FILE=$INI \
        HOME=/ TSLIB_FBDEVICE=/dev/fb0 TSLIB_CONSOLEDEVICE=none TERM=vt102 \
        PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin:/usr/local/app/:/usr/local/bin/wifi/ \
        TSLIB_CONFFILE=/usr/local/app/ts.config SHELL=/bin/sh PWD=/usr/local/app \
        interDebug $APP 9527 </dev/console >/dev/console 2>&1 &
}
start_mainapp_checked() {      # patched build not running after 10 s -> next one down (intercom must keep working)
    start_mainapp
    [ "$APP" = $STOCK ] && return
    sleep 10
    pidof mainapp >/dev/null && { bootlog INFO "$(date) mainapp: $APP (ini $INI)"; return; }
    if [ "$APP" = $HQ ]; then
        bootlog WARNING "$(date) HQ mainapp did not start: normal video (app.mode set to std)"
        echo std > $D/app.mode; sync
        APP=$KEY; INI=$KEY_INI
        start_mainapp_checked
        return
    fi
    bootlog WARNING "$(date) patched mainapp did not start: falling back to stock"
    APP=$STOCK; INI=$STOCK_INI
    start_mainapp
}
app_switch() {                 # request from HA via the bridge (only sent in idle / viewing): hq | std
    want=$(cat $R/app.want 2>/dev/null); rm -f $R/app.want
    case "$want" in hq|std) ;; *) bootlog WARNING "$(date) app.want: ignored '$want'"; return ;; esac
    echo $want > $D/app.mode.tmp && mv $D/app.mode.tmp $D/app.mode && sync
    old=$APP; pick_app
    if [ "$want" = hq ] && [ "$APP" != $HQ ]; then bootlog WARNING "$(date) video hq requested, but $HQ is not installed"; fi
    [ "$APP" = "$old" ] && { bootlog INFO "$(date) video $want: already running ($APP)"; return; }
    bootlog INFO "$(date) video $want: restarting mainapp ($APP)"
    stop_mainapp
    bridge_reset
    start_mainapp_checked
}
stop_mainapp() {
    kill $(pidof interDebug) $(pidof mainapp) 2>/dev/null
    i=0; while pidof mainapp >/dev/null && [ $i -lt 20 ]; do usleep 100000; i=$((i+1)); done
    pidof mainapp >/dev/null && kill -9 $(pidof mainapp)
}
# mainapp is stopped: the bridge forgets the live session / call it was serving and puts call audio and the door
# camera off on the bus. A killed mainapp never sends those frames itself (only its own close path does), so
# without this the outdoor camera stayed on and the bridge skipped the camera-off at the end of later calls.
bridge_reset() {
    : > $R/mainapp.reset
    i=0; while [ -e $R/mainapp.reset ] && [ $i -lt 50 ]; do usleep 100000; i=$((i+1)); done
    rm -f $R/mainapp.reset      # bridge not answering (5 s): do not leave a stale request behind
    usleep 500000               # the bridge sends 43 00 + 41 right after taking the request
}
# "Reboot monitor" button in HA -> bridge -> $R/reboot.want. Mainapp is stopped cleanly first, the door camera is
# put off, then the monitor reboots (everything in RAM, including this request, is gone afterwards).
do_reboot() {
    rm -f $R/reboot.want
    bootlog WARNING "$(date) reboot requested from HA: stopping mainapp, camera off, reboot"
    stop_mainapp
    bridge_reset
    sync
    reboot || bootlog ERROR "$(date) reboot command failed"
    sleep 30                    # a real reboot kills this script long before; still here = it did not happen
    bootlog ERROR "$(date) still running 30 s after the reboot request: restarting mainapp"
    start_mainapp_checked
}
LUA_O=/usr/local/app/luaprj/src/base/base.lua
LUA_ORIG=77d3402119e91d34042874b2a776fc93; LUA_PATCHED=88d80b8b01aea3c9d2f1eb3f55c1c627
lua_on=0
lua_patch() {                  # only the analysed original and the device-tested patch (T54); else stock Lua
    [ -f $D/lua/base.lua ] && [ ! -e $D/disable-lua ] || return
    [ "$(md5sum $LUA_O | cut -c1-32)" = $LUA_ORIG ] || { bootlog WARNING "$(date) Lua patch: original base.lua differs, not applied"; return; }
    [ "$(md5sum $D/lua/base.lua | cut -c1-32)" = $LUA_PATCHED ] || { bootlog WARNING "$(date) Lua patch: $D/lua/base.lua unexpected, not applied"; return; }
    mount --bind $D/lua/base.lua $LUA_O && { lua_on=1; bootlog INFO "$(date) Lua patch applied (base.lua $LUA_PATCHED)"; } \
        || bootlog WARNING "$(date) Lua patch: bind mount failed, stock Lua"
}
mainapp_has_tty() {
    p=$(pidof mainapp) || return 1
    ls -l /proc/$p/fd 2>/dev/null | grep -q "$1"
}

# 1. wait (max 180 s) until mainapp runs and holds the real UART, then let it settle
i=0; while ! mainapp_has_tty '/dev/ttySGK1$' && [ $i -lt 180 ]; do sleep 1; i=$((i+1)); done
mainapp_has_tty '/dev/ttySGK1$' || { bootlog ERROR "mainapp never opened ttySGK1, abort (stock)"; exit 1; }
sleep 15
bootlog INFO "$(date) mainapp up after ${i}s, swapping"

# 2. swap: bridge on a pty in place of the UART, mainapp restarted behind it
mv /dev/ttySGK1 /dev/ttySGK1.real
setsid $D/vimarbridge /dev/ttySGK1.real /dev/ttySGK1 $D/bridge.conf </dev/null >/dev/null 2>>$R/bridge.log &
sleep 1
if ! [ -L /dev/ttySGK1 ] || ! pidof vimarbridge >/dev/null; then
    bootlog ERROR "bridge did not start, restoring (mainapp untouched)"
    killall vimarbridge 2>/dev/null; rm -f /dev/ttySGK1; mv /dev/ttySGK1.real /dev/ttySGK1
    exit 1
fi
lua_patch
stop_mainapp
start_mainapp_checked
sleep 5
if [ $lua_on = 1 ] && ! pidof mainapp >/dev/null; then     # never seen; the intercom must keep working
    bootlog WARNING "$(date) mainapp not running with the Lua patch: removing it, restarting"
    umount $LUA_O; lua_on=0; start_mainapp_checked; sleep 5
fi
mainapp_has_tty 'pts' && bootlog INFO "$(date) OK: mainapp behind the bridge" || bootlog WARNING "$(date) WARNING: mainapp not on the pty"

# 3. optional media helper (vimaraudio): gated by /tmp/vb/audio.state, written by the bridge
HA_IP=$(sed -n 's/^ha_ip=//p' $D/media.conf 2>/dev/null | head -1)
media_ok=0
if [ -x $D/vimaraudio ] && [ -n "$HA_IP" ] && [ ! -e $D/disable-audio ]; then media_ok=1; fi
start_media() {
    [ -e $R/audio.state ] || printf '0\n' > $R/audio.state
    setsid $D/vimaraudio "$HA_IP" 8554 $R/audio.state </dev/null >>$R/audio.log 2>&1 &
    bootlog INFO "$(date) vimaraudio started (peer $HA_IP)"
}
[ $media_ok = 1 ] && start_media
restarts=0
# logs live in RAM: keep each under 1 MB (last 256 KB kept; same file, writers append)
trim() { s=$(wc -c < "$1" 2>/dev/null || echo 0); [ "$s" -gt 1048576 ] && { tail -c 262144 "$1" > "$1.t" && cat "$1.t" > "$1"; rm -f "$1.t"; }; }

# 4. supervisor: video quality switch and reboot request (every 2 s); a mainapp that stopped or was replaced
#    without the bridge noticing (crash) gets the same bridge reset; restart the media helper if it dies (max 5);
#    if the bridge dies, go back to stock
rm -f $R/app.want $R/reboot.want
mp=$(pidof mainapp)
n=0
while pidof vimarbridge >/dev/null; do
    sleep 2
    cur=$(pidof mainapp)
    if [ -n "$mp" ] && [ "$cur" != "$mp" ]; then
        bootlog WARNING "$(date) mainapp stopped or restarted on its own: bridge reset (camera off)"
        bridge_reset
    fi
    mp=$cur
    [ -e $R/reboot.want ] && do_reboot
    if [ -e $R/app.want ]; then app_switch; mp=$(pidof mainapp); fi
    n=$((n+1)); [ $n -lt 5 ] && continue; n=0
    for f in $R/bridge.log $R/audio.log $R/boot.log; do trim $f; done
    if [ $media_ok = 1 ] && ! pidof vimaraudio >/dev/null; then
        if [ $restarts -lt 5 ]; then restarts=$((restarts+1)); bootlog WARNING "$(date) vimaraudio died, restart $restarts/5"; start_media
        else bootlog ERROR "$(date) vimaraudio died too often, leaving it off"; media_ok=0; fi
    fi
done
bootlog ERROR "$(date) bridge died: restoring stock UART and mainapp"
killall vimaraudio 2>/dev/null; printf '0\n' > $R/audio.state
stop_mainapp
rm -f /dev/ttySGK1; mv /dev/ttySGK1.real /dev/ttySGK1
start_mainapp
bootlog INFO "$(date) stock restored"
