#!/bin/sh
# Persistent install of vimarbridge + vimaraudio on /mntfs/vimarbridge (data partition only; rootfs untouched).
# Run on the PC. The existing hook in /mntfs/dropbear/start.sh starts boot.sh at boot (T25).
#
#   ./install.sh install <dir> <ha_ip>   back up current files as *.prev, install vimarbridge, vimaraudio, boot.sh, media.conf
#   ./install.sh verify                  after a reboot: boot.log, processes, audio log tail
#   ./install.sh rollback                restore *.prev, remove vimaraudio + media.conf (takes effect at next boot)
#   ./install.sh audio-off|audio-on      disable/enable only the media helper at next boot
#   ./install.sh app keylock|hq <dir>    install a patched mainapp (<dir>/mainapp, e.g. from keylock/make_keylock.py) in /mntfs/vimarbridge/app
#   ./install.sh app stock               remove it (stock mainapp from next boot); boot.sh falls back to stock by itself
#                                        if the patched build does not start
H=${VIMAR_SSH:-vimar}
D=/mntfs/vimarbridge
case "$1" in
install)
    dir=${2:?dir}; ip=${3:?ha_ip}
    for f in vimarbridge vimaraudio boot.sh; do [ -f "$dir/$f" ] || { echo "missing $dir/$f"; exit 1; }; done
    ssh $H "[ -d $D ] && [ -f $D/bridge.conf ] && [ -f $D/broker.pem ]" || { echo "$D incomplete (bridge.conf/broker.pem) - stop"; exit 1; }
    for f in vimarbridge vimaraudio boot.sh; do
        ssh $H "cat > $D/$f.new && chmod 755 $D/$f.new" < "$dir/$f" || exit 1
    done
    ssh $H "cd $D
        for f in vimarbridge boot.sh vimaraudio media.conf; do [ -e \$f ] && cp -p \$f \$f.prev; done
        mv vimarbridge.new vimarbridge && mv boot.sh.new boot.sh && mv vimaraudio.new vimaraudio
        printf 'ha_ip=$ip\n' > media.conf && chmod 644 media.conf
        rm -f disable disable-audio
        sync
        echo '--- installed:'; sha256sum vimarbridge vimaraudio boot.sh | cut -c1-8,65-; cat media.conf
        echo '--- backups:'; ls -l *.prev 2>/dev/null
        echo '--- boot hook:'; grep vimarbridge /mntfs/dropbear/start.sh"
    echo "Now reboot the monitor (power cycle or: ssh $H reboot), wait ~4 min, then: $0 verify"
    ;;
verify)
    ssh $H "echo \"uptime: \$(cut -d. -f1 /proc/uptime) s\"; tail -6 /tmp/vb/boot.log
        echo \"bridge: \$(pidof vimarbridge)  audio: \$(pidof vimaraudio)  mainapp: \$(pidof mainapp)  audio.state: \$(cat /tmp/vb/audio.state 2>/dev/null)\"
        ls -l /dev/ttySGK1; tail -3 /tmp/vb/audio.log 2>/dev/null
        echo \"mainapp exe: \$(readlink /proc/\$(pidof mainapp)/exe)\"; grep 'mainapp:' /tmp/vb/boot.log | tail -1"
    ;;
rollback)
    ssh $H "cd $D
        for f in vimarbridge boot.sh; do [ -e \$f.prev ] && cp -p \$f.prev \$f; done
        if [ -e vimaraudio.prev ]; then cp -p vimaraudio.prev vimaraudio; else rm -f vimaraudio; fi
        if [ -e media.conf.prev ]; then cp -p media.conf.prev media.conf; else rm -f media.conf; fi
        sync; sha256sum vimarbridge boot.sh | cut -c1-8,65-; ls -l"
    echo "Rolled back; reboot the monitor to apply."
    ;;
app)
    mode=${2:?keylock|hq|stock}
    if [ "$mode" = stock ]; then
        ssh $H "rm -rf $D/app && sync && echo 'patched mainapp removed: stock from next boot'"
    else
        dir=${3:?dir}; [ -f "$dir/mainapp" ] || { echo "missing $dir/mainapp"; exit 1; }
        [ "$mode" = hq ] && { [ -f "$dir/goke_prm_hq.ini" ] || { echo "missing $dir/goke_prm_hq.ini"; exit 1; }; }
        [ -f "$dir/boot.sh" ] || { echo "missing $dir/boot.sh (needs the T34 version with app/ support)"; exit 1; }
        grep -q 'start_mainapp_checked' "$dir/boot.sh" || { echo "$dir/boot.sh is too old (no app/ support)"; exit 1; }
        ssh $H "cd $D && cp -p boot.sh boot.sh.prev && cat > boot.sh.new && chmod 755 boot.sh.new && mv boot.sh.new boot.sh" < "$dir/boot.sh" || exit 1
        ssh $H "mkdir -p $D/app.new && cat > $D/app.new/mainapp && chmod 755 $D/app.new/mainapp" < "$dir/mainapp" || exit 1
        [ "$mode" = hq ] && { ssh $H "cat > $D/app.new/goke_prm.ini" < "$dir/goke_prm_hq.ini" || exit 1; }
        ssh $H "rm -rf $D/app.prev; [ -d $D/app ] && mv $D/app $D/app.prev; mv $D/app.new $D/app && sync
            echo '--- installed ($mode):'; sha256sum $D/app/* | cut -c1-8,65-; df -h /mntfs | tail -1"
    fi
    echo "Reboot the monitor to apply (ssh $H reboot), then: $0 verify"
    ;;
audio-off) ssh $H "touch $D/disable-audio && sync && echo 'media helper off from next boot'" ;;
audio-on)  ssh $H "rm -f $D/disable-audio && sync && echo 'media helper on from next boot'" ;;
*) sed -n 2,9p "$0"; exit 2 ;;
esac
