#!/bin/sh
# RAM-only switch-over: put vimarbridge between mainapp and the ACB UART.
# Run on a freshly booted (stock) monitor. UNDO = reboot.
# usage: sh /tmp/vb/swap_vb.sh        (expects /tmp/vb/vimarbridge, bridge.conf, broker.pem)
# Starts the persistent patched mainapp (T33 key lock) from /mntfs/vimarbridge/app if present, like boot.sh:
# a stock mainapp started after the stream key's SyncTime would rotate the key (T33).
D=/tmp/vb
P=/mntfs/vimarbridge
APP=/usr/local/app/mainapp; INI=/usr/local/config/goke_prm.ini
if [ -x $P/app/mainapp ] && [ ! -e $P/disable-app ]; then
    APP=$P/app/mainapp; [ -f $P/app/goke_prm.ini ] && INI=$P/app/goke_prm.ini
fi
for f in vimarbridge bridge.conf broker.pem; do [ -e $D/$f ] || { echo "missing $D/$f"; exit 1; }; done
if pidof ringproxy >/dev/null || pidof vimarbridge >/dev/null || [ -e /dev/ttySGK1.real ]; then
    echo "a proxy is already active - reboot the monitor first, then rerun"; exit 1
fi

mv /dev/ttySGK1 /dev/ttySGK1.real
setsid $D/vimarbridge /dev/ttySGK1.real /dev/ttySGK1 $D/bridge.conf -v </dev/null >/dev/null 2>$D/bridge.log &
sleep 1
if ! [ -L /dev/ttySGK1 ] || ! pidof vimarbridge >/dev/null; then
    echo "bridge did not start - restoring, mainapp untouched"
    killall vimarbridge 2>/dev/null; rm -f /dev/ttySGK1; mv /dev/ttySGK1.real /dev/ttySGK1
    cat $D/bridge.log; exit 1
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
sleep 4
if [ "$APP" != /usr/local/app/mainapp ]; then          # patched build not running: stock, the intercom must keep working
    sleep 6
    if ! pidof mainapp >/dev/null; then
        echo "patched mainapp did not start: starting stock (reboot soon if the stream key's SyncTime has passed)"
        APP=/usr/local/app/mainapp; INI=/usr/local/config/goke_prm.ini
        setsid env -i \
            TSLIB_ROOT=/usr/local TSLIB_TSDEVICE=/dev/input/event0 USER=root \
            LD_LIBRARY_PATH=/lib:/usr/lib GOKE_CONFIG_FILE=$INI \
            HOME=/ TSLIB_FBDEVICE=/dev/fb0 TSLIB_CONSOLEDEVICE=none TERM=vt102 \
            PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin:/usr/local/app/:/usr/local/bin/wifi/ \
            TSLIB_CONFFILE=/usr/local/app/ts.config SHELL=/bin/sh PWD=/usr/local/app \
            interDebug $APP 9527 </dev/console >/dev/console 2>&1 &
        sleep 4
    fi
fi
echo "--- mainapp: $APP (ini $INI), exe $(readlink /proc/$(pidof mainapp)/exe 2>/dev/null)"
echo "--- mainapp now uses:"; ls -l /proc/$(pidof mainapp)/fd 2>/dev/null | grep -E 'pts|ttySGK|watchdog'
echo "--- bridge log:"; cat $D/bridge.log
