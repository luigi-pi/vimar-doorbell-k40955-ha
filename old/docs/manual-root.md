# Manual method — root access and backup, typed by hand

This is the hand‑typed version of [step 2](02-root-and-backup.md). The helper scripts
(`tools/vimar.py`, `on-device/ssh/ram-ssh.sh`, `on-device/ssh/make-permanent.sh`) run these same
commands and check each result. Use this page if you want to understand every step, or if a script
stops on your monitor.

You need the serial console from [step 1](01-serial-console.md), the program `dist/dropbearmulti`
(from the kit zip, or [built yourself](building.md)) and your SSH key (`ssh-keygen -t ed25519`).

| Placeholder | What it is |
|---|---|
| `192.168.111.1` | the monitor's address in recovery mode (set by its `go_ap.sh`) |
| `<PC-IP>` | your PC's address on the monitor's Wi‑Fi, usually `192.168.111.100` (Windows: `ipconfig`, Linux: `ip -br addr`) |
| `<MONITOR_IP>` | the monitor's address on your home network after a normal start |

## A. Recovery mode and Wi‑Fi

Exactly [steps 1 and 2](02-root-and-backup.md#step-1--start-the-recovery-mode) of the main guide:
stop at `I.mx6.UL#`, enter the three U‑Boot lines, then the block that ends with `./go_ap.sh`, and
join the monitor's Wi‑Fi with the PC.

## B. Temporary SSH server in RAM (nothing written)

**On the PC**, serve the program and your public key:

**Windows** (Command Prompt, in the kit folder)
```
cd dist
copy %USERPROFILE%\.ssh\id_ed25519.pub .
python -m http.server 8000
```
**Linux**
```sh
cd dist
cp ~/.ssh/id_ed25519.pub .
python3 -m http.server 8000
```

**On the monitor:**

```sh
mkdir -p /tmp/ssh
cd /tmp/ssh
wget http://<PC-IP>:8000/dropbearmulti
wget http://<PC-IP>:8000/id_ed25519.pub -O authorized_keys
chmod +x dropbearmulti
ln -sf dropbearmulti dropbear
ln -sf dropbearmulti dropbearkey
chmod 600 authorized_keys
./dropbearkey -t ed25519 -f /tmp/ssh/dropbear_ed25519_host_key
```

Create the start script. Paste the whole block as one piece:

```sh
cat > /tmp/ssh/start.sh <<'EOF'
#!/bin/sh
D=/tmp/ssh
[ -d /dev/pts ] || mkdir -p /dev/pts
mount | grep -q devpts || mount -t devpts devpts /dev/pts 2>/dev/null
[ -e /dev/ptmx ] || mknod /dev/ptmx c 5 2
chmod 666 /dev/ptmx 2>/dev/null
"$D/dropbearmulti" dropbear -E -s -p 22 -r "$D/dropbear_ed25519_host_key" -D "$D" -P /var/run/dropbear.pid &
EOF
chmod 755 /tmp/ssh/start.sh
sh /tmp/ssh/start.sh
```

Stop the HTTP server on the PC (`Ctrl+C`) and log in from the PC:

```
ssh root@192.168.111.1
```

A root prompt (it may be blank — type `id`, it prints `uid=0(root)`) means it works. If it says
*Permission denied*, download `authorized_keys` again with the `wget … -O authorized_keys` line (do
not paste the key by hand).

## C. Back up the flash (before anything is written)

**On the monitor**, list the partitions and create the device node of the one called `all`
(the whole chip; on this kit `mtd5`). The node numbers are always `90` and twice the partition number:

```sh
cat /proc/mtd
mknod /dev/mtd5 c 90 10
```

**On the PC**, copy it, then compare the checksum the monitor prints with the checksum of the copy —
they must be equal:

**Windows** (Command Prompt — **not** PowerShell, which damages the file)
```
ssh root@192.168.111.1 "cat /dev/mtd5" > nand_all.bin
ssh root@192.168.111.1 "sha256sum /dev/mtd5"
certutil -hashfile nand_all.bin SHA256
```
**Linux**
```sh
ssh root@192.168.111.1 "cat /dev/mtd5" > nand_all.bin
ssh root@192.168.111.1 "sha256sum /dev/mtd5"
sha256sum nand_all.bin
```

`nand_all.bin` must be exactly 134217728 bytes (the size `08000000` shown by `cat /proc/mtd`). If the
monitor has no `sha256sum`, copy the flash a second time under another name and compare the two files.
Keep the backup safe and private; do not continue without it.

## D. Mount the monitor's writable storage

The SSH server will live on the data partition (`mtd4`), which a normal start mounts at `/mntfs`.
In recovery mode mount it by hand. **On the monitor:**

```sh
mknod /dev/ubi_ctrl c 10 61
cd /usr/local/bin
./ubiattach -m 4 -d 4 /dev/ubi_ctrl
cat /sys/class/ubi/ubi4_0/dev      # prints e.g. 250:1 (major:minor)
mknod /dev/ubi4_0 c 250 1          # use YOUR two numbers if different
mount -t ubifs ubi4_0 /mntfs
ls /mntfs                          # should list: wifi checksum storage ...
```

## E. Copy the SSH server to the data partition

```sh
mkdir -p /mntfs/dropbear
cd /mntfs/dropbear
cp /tmp/ssh/dropbearmulti /tmp/ssh/authorized_keys /tmp/ssh/dropbear_ed25519_host_key .
ln -sf dropbearmulti dropbear
ln -sf dropbearmulti dropbearkey
chmod 600 authorized_keys dropbear_ed25519_host_key
sed 's#^D=.*#D=/mntfs/dropbear#' /tmp/ssh/start.sh > start.sh
chmod 755 start.sh dropbearmulti
sync
```

## F. Start it automatically (the one flash write)

Save the original boot script (your undo file), make the root filesystem writable, add the line,
flush, and lock it again:

```sh
cp /root/boot_app.sh /mntfs/dropbear/boot_app.sh.orig
mount -o remount,rw /
sed -i '/^cmdapp &/i [ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &' /root/boot_app.sh
sync
mount -o remount,ro /
```

Check that exactly this one line was added, right before `cmdapp &`, and nothing else changed:

```sh
diff /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh
```

```
[ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &
```

> If `diff` shows anything else, undo it now while you still have this shell:
> `mount -o remount,rw / && cp /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh && sync && mount -o remount,ro /`

## G. Normal start

Switch the monitor off and on **without touching the keyboard**, put the PC back on your home Wi‑Fi,
find the monitor in your router and log in:

```
ssh root@<MONITOR_IP>
```

## Undo (recovery)

A broken or unwanted `boot_app.sh` needs no reflash. In recovery mode (A) with the data partition
mounted (D):

```sh
mount -o remount,rw /
cp /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh
sync
mount -o remount,ro /
```

Power‑cycle: the monitor is back to stock. On a *normal* start the serial console is silent (the
monitor's own program holds it), so a failed auto‑start can only be fixed from recovery mode — which
is why the login is proven in B before F wires it in.
