#!/bin/sh
# RAM-only test of the T30 set (vimarbridge + vimaraudio v2) on the K40955. Run on the PC, not on the monitor.
# Nothing is written to /mntfs. Undo = "./ramtest.sh undo" (reboot: the persistent install comes back as before).
#
#   ./ramtest.sh push <dir>        copy vimarbridge, vimaraudio, aicap, swap_vb.sh from <dir> to /tmp/vb on the monitor
#   ./ramtest.sh swap              stop the persistent bridge (boot.sh restores stock), start the new one from RAM
#   ./ramtest.sh audio <ha_ip>     (re)start vimaraudio from RAM (keeps the bridge's audio.state), peer = go2rtc's address
#   ./ramtest.sh status            processes, audio.state, open audio fds, AO/AI lines from the logs
#   ./ramtest.sh watch             follow bridge + audio logs (Ctrl-C to stop)
#   ./ramtest.sh logs <dir>        fetch all logs into <dir>
#   ./ramtest.sh mainapp keylock|hq [dir]  restart only mainapp with <dir>/mainapp built by keylock/make_keylock.py
#                                   (hq also uses <dir>/goke_prm_hq.ini; T33/T34)
#   ./ramtest.sh mainapp stock     restart only mainapp stock (or: undo = reboot)
#   ./ramtest.sh undo              reboot the monitor
H=${VIMAR_SSH:-vimar}
case "$1" in
push)
    d=${2:?dir}
    ssh $H 'mkdir -p /tmp/vb'
    for f in vimarbridge vimaraudio aicap swap_vb.sh; do
        ssh $H "cat > /tmp/vb/$f.new && chmod 755 /tmp/vb/$f.new && mv /tmp/vb/$f.new /tmp/vb/$f" < "$d/$f" || exit 1
    done
    ssh $H 'cp /mntfs/vimarbridge/bridge.conf /mntfs/vimarbridge/broker.pem /tmp/vb/ && chmod 600 /tmp/vb/bridge.conf
            cd /tmp/vb && sha256sum vimarbridge vimaraudio aicap | cut -c1-8,65-'
    ;;
swap)
    ssh $H 'pidof vimaraudio >/dev/null && killall vimaraudio
            if pidof vimarbridge >/dev/null; then
                if ps | grep -v grep | grep -q "vimarbridge/boot.sh"; then
                    killall vimarbridge; echo "waiting for boot.sh to restore stock (max 30 s)..."
                    i=0; while [ -e /dev/ttySGK1.real ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
                    sleep 5
                else
                    echo "bridge has no boot.sh supervisor (earlier RAM test): stopping it and putting the UART back"
                    killall vimarbridge; sleep 1
                fi
            fi
            if [ -e /dev/ttySGK1.real ]; then          # no supervisor restored it: do it here; swap_vb.sh restarts mainapp
                rm -f /dev/ttySGK1 && mv /dev/ttySGK1.real /dev/ttySGK1 || { echo "cannot restore UART - run: ./ramtest.sh undo"; exit 1; }
                echo "UART node restored"
            fi
            tail -3 /tmp/vb/boot.log 2>/dev/null
            sh /tmp/vb/swap_vb.sh'
    ;;
audio)
    ip=${2:?ha_ip}
    ssh $H 'pidof vimarbridge >/dev/null && [ -L /dev/ttySGK1 ]' || { echo "no bridge running - run ./ramtest.sh swap first"; exit 1; }
    ssh $H "killall vimaraudio 2>/dev/null; [ -e /tmp/vb/audio.state ] || printf '0\n' > /tmp/vb/audio.state
            setsid /tmp/vb/vimaraudio $ip 8554 /tmp/vb/audio.state </dev/null >>/tmp/vb/audio.log 2>&1 &
            sleep 1; pidof vimaraudio && tail -2 /tmp/vb/audio.log"
    ;;
status)
    ssh $H 'echo "uptime: $(cut -d. -f1 /proc/uptime) s  uart: $(ls -l /dev/ttySGK1 | sed "s/.* //") $( [ -e /dev/ttySGK1.real ] && echo "(.real present)")"
            echo "bridge: $(pidof vimarbridge)  audio: $(pidof vimaraudio)  mainapp: $(pidof mainapp)  audio.state: $(cat /tmp/vb/audio.state 2>/dev/null)"
            for p in $(pidof vimaraudio) $(pidof mainapp); do echo "pid $p audio fds:"; ls -l /proc/$p/fd 2>/dev/null | grep -E "a[io]_dev" || echo "  none"; done
            echo "--- audio.log (last 16 lines)"; tail -16 /tmp/vb/audio.log 2>/dev/null
            echo "--- bridge.log (state lines)"; grep -E "state:|relay:" /tmp/vb/bridge.log | tail -12'
    ;;
mainapp)
    # RAM-only test of a patched mainapp (T33/T34): restart only mainapp (bridge keeps running);
    # output goes to /dev/console as in the stock start (mainapp needs a tty)
    mode=${2:?keylock|hq|stock}; d=${3:-.}
    if [ "$mode" = keylock ] || [ "$mode" = hq ]; then
        [ -f "$d/mainapp" ] || { echo "missing $d/mainapp (build it with keylock/make_keylock.py)"; exit 1; }
        ssh $H 'pidof vimarbridge >/dev/null && [ -L /dev/ttySGK1 ]' || { echo "bridge not running"; exit 1; }
        # keep the file name "mainapp": pidof/kill (here and in boot.sh) match the process name
        ssh $H "mkdir -p /tmp/vb/hq && rm -f /tmp/vb/mainapp.hq && cat > /tmp/vb/hq/mainapp.new && chmod 755 /tmp/vb/hq/mainapp.new && mv /tmp/vb/hq/mainapp.new /tmp/vb/hq/mainapp" < "$d/mainapp" || exit 1
        BIN=/tmp/vb/hq/mainapp; INI=/usr/local/config/goke_prm.ini
        if [ "$mode" = hq ]; then
            [ -f "$d/goke_prm_hq.ini" ] || { echo "missing $d/goke_prm_hq.ini"; exit 1; }
            ssh $H "cat > /tmp/vb/goke_prm_hq.ini" < "$d/goke_prm_hq.ini" || exit 1
            INI=/tmp/vb/goke_prm_hq.ini
        fi
    else
        BIN=/usr/local/app/mainapp; INI=/usr/local/config/goke_prm.ini
    fi
    ssh $H "sha256sum $BIN | cut -c1-8
        killall interDebug mainapp mainapp.hq 2>/dev/null
        i=0; while pidof mainapp >/dev/null && [ \$i -lt 20 ]; do usleep 100000; i=\$((i+1)); done
        pidof mainapp >/dev/null && kill -9 \$(pidof mainapp); killall -9 mainapp.hq 2>/dev/null
        cd /usr/local/app
        setsid env -i TSLIB_ROOT=/usr/local TSLIB_TSDEVICE=/dev/input/event0 USER=root \
            LD_LIBRARY_PATH=/lib:/usr/lib GOKE_CONFIG_FILE=$INI \
            HOME=/ TSLIB_FBDEVICE=/dev/fb0 TSLIB_CONSOLEDEVICE=none TERM=vt102 \
            PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin:/usr/local/app/:/usr/local/bin/wifi/ \
            TSLIB_CONFFILE=/usr/local/app/ts.config SHELL=/bin/sh PWD=/usr/local/app \
            interDebug $BIN 9527 </dev/console >/dev/console 2>&1 &   # needs a tty (tcgetattr), like the stock start
        sleep 12
        p=\$(pidof mainapp); echo \"mainapp pid \$p exe \$(readlink /proc/\$p/exe) uart \$(ls -l /proc/\$p/fd | grep -c pts) pts fd\"
        [ -n \"\$p\" ] || echo 'mainapp NOT running - reboot the monitor (ssh vimar reboot)'"
    ;;
watch)
    ssh $H 'tail -f /tmp/vb/bridge.log /tmp/vb/audio.log'
    ;;
logs)
    d=${2:?dir}; mkdir -p "$d"
    for f in bridge.log audio.log boot.log; do ssh $H "cat /tmp/vb/$f 2>/dev/null" > "$d/$f"; done; ls -l "$d"
    ;;
undo)
    ssh $H reboot
    ;;
*)
    sed -n 2,12p "$0"; exit 2
    ;;
esac
