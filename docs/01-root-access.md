# 1 — Permanent SSH root on the monitor

A step-by-step recipe to install a permanent, cloud-free **SSH root** login on
the Vimar K40955 monitor (Goke GK7101S). After this, the monitor runs an SSH
server that starts by itself on every power-on, and you log in from a PC on
your home network with a key — no serial cable, no cloud.

This procedure is written so it can
be followed without deep Linux experience. **Read it once before starting.**

> **Prerequisite:** you must already have UART/U-Boot access to the monitor
> — a 3.3 V USB-serial
> adapter on the `GND TX RX` header and a way to reach the U-Boot prompt. This
> guide picks up from there.
>
> **Risk:** step 6 writes **one line** to the monitor's flash. It is low-risk and
> reversible, but a flash write is never zero-risk. **Do step 1 (full backup)
> first** — it is the safety net. The indoor intercom keeps working normally
> throughout (G6).

---

## What you need

- A **Linux PC** (these instructions use Debian; `apt`).
- The monitor's **UART** working (serial adapter + terminal), per the prerequisite.
- A way to give the PC internet **and** reach the monitor at the same time. The
 monitor, in its recovery `/bin/sh` boot, runs its own Wi-Fi access point, so
 the simplest setup is: PC internet over a **USB phone tether**, and the PC's
 **Wi-Fi** joined to the monitor's access point. (On a normal boot the monitor
 instead joins your home Wi-Fi — that is how you reach it at the end.)

### Placeholders used below — replace each with your own value

| Placeholder | What it is | Where to get it |
|---|---|---|
| `<AP-SSID>` | the monitor's own Wi-Fi name in recovery mode | printed by `go_ap.sh`; e.g. read `/usr/local/bin/wifi/hostapd.conf` on the monitor |
| `<PC-IP-ON-AP>` | your PC's address on the monitor's access point | `ip -br addr show <wifi-iface>` after joining (often `192.168.111.100`) |
| `<AP-IP>` | the monitor's address in recovery mode | `192.168.111.1` on this kit (set by `go_ap.sh`) |
| `<LAN-IP>` | the monitor's address on your home network, normal boot | your router's device list, by the monitor's MAC |

Keep the monitor's label values (UID, back-label password, MAC, AP name) out of
any file you commit — they belong in a git-ignored `.env`.

---

## Step 1 — Back up the flash first (do not skip)

The only safe recovery for a bad write is a full flash image. Take it before
anything else, with the monitor in a root shell (U-Boot → `init=/bin/sh`, see
step 3 for how to reach that shell). Dump each partition and the spare/OOB area,
then copy them to the PC and verify. Keep them off-box; they are large and
sensitive, so `.gitignore` already excludes `*.bin`/`*.img`.

```sh
# on the monitor (root shell):
cat /proc/mtd # note the partition numbers: rootfs=mtd3, data=mtd4, all=mtd5
```

Dump the whole chip and the partitions you will touch, then `sha256sum` them and
copy to the PC (e.g. over the Wi-Fi link with a small HTTP transfer). Record the
checksums. A dump with the OOB/spare area is the gold standard for NAND. Do not
continue until you have verified copies on the PC.

> This guide does not cover *writing* the backup back, because on this kit the
> only restore path is an untested U-Boot `snand write`. In practice you will
> almost never need it: the real recovery for the one edit in this guide is step 8,
> which needs no reflash. The backup is the last-resort insurance.

---

## Step 2 — Build the SSH server (on the PC)

The monitor's CPU is an **ARM1176 / ARMv6, soft-float** core. (Its `/proc/cpuinfo`
confusingly says `CPU architecture: 7`, but the part is ARM1176 — build for
**armv6, not armv7**.) Two non-obvious flags are required or the login will not
work; both matter (see the summary table).

Install the cross-compiler:

```sh
sudo apt update
sudo apt install -y gcc-arm-linux-gnueabi
```

Get the dropbear source:

```sh
cd ~
wget https://matt.ucc.asn.au/dropbear/releases/dropbear-2025.89.tar.bz2
tar xjf dropbear-2025.89.tar.bz2
cd dropbear-2025.89
```

Configure it pubkey-only (no passwords) and turn off the server-side extension:

```sh
cat > localoptions.h <<'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM 0
EOF
sed -i 's/^#define DROPBEAR_EXT_INFO.*/#define DROPBEAR_EXT_INFO 0/' src/sysoptions.h
sed -i '/^#define DROPBEAR_EXT_INFO 0/{n;/DROPBEAR_CLI_PUBKEY_AUTH/d}' src/sysoptions.h
```

Patch out dropbear's key-file permission check. The monitor's firmware ships `/`
as world-writable, which makes that check reject every key; disabling it is
correct on this single-user appliance :

```sh
sed -i 's/\tTRACE(("enter checkpubkeyperms"))/\tTRACE(("enter checkpubkeyperms"))\n\tm_free(path); return DROPBEAR_SUCCESS; \/* 0777-rootfs device *\//' src/svr-authpubkey.c
```

Build it, static, for armv6 soft-float, and strip it:

```sh
make clean
./configure --host=arm-linux-gnueabi --disable-zlib --disable-harden \
 CC=arm-linux-gnueabi-gcc \
 CFLAGS="-O1 -march=armv6 -marm -mfloat-abi=soft -fno-strict-aliasing -fno-strict-overflow -mno-unaligned-access" \
 LDFLAGS="-static"
make PROGRAMS="dropbear dropbearkey" MULTI=1 STATIC=1
arm-linux-gnueabi-strip dropbearmulti
```

Check the result — it **must** say `ARM` and `statically linked`:

```sh
file dropbearmulti
```

> If `file` says "dynamically linked" or the build errors, stop — something in
> the flags was dropped. The two flags that matter most are
> `-fno-strict-aliasing` (without it the SSH handshake fails with "no matching
> cipher") and `-march=armv6` (not armv7).

Make your login key, if you do not already have one:

```sh
ls ~/.ssh/id_ed25519 2>/dev/null || ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519
```

---

## Step 3 — Boot the monitor into a root shell (recovery boot)

Power-cycle the monitor and interrupt U-Boot (flood **Enter** during the ~2 s
countdown) to reach the prompt `I.mx6.UL#`, then boot a root shell in RAM —
nothing here is written to flash:

```
setenv bootargs console=ttySGK0,115200 ubi.mtd=3 rootfstype=ubifs root=ubi0:rootfs ro init=/bin/sh mem=90M
snand read 0xC1000000 0x100000 0x200000
bootm
```

At the `/ #` shell, set up the basics and bring up the monitor's Wi-Fi access
point (this mirrors the stock boot; run it line by line):

```sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /dev
mkdir -p /dev/pts
mknod /dev/null c 1 3
mknod /dev/ttySGK0 c 204 18
chmod 666 /dev/null /dev/ttySGK0
mount -t tmpfs -o size=8m tmpfs /tmp
insmod /usr/local/bin/wifi/rtl8188fu.ko
mkdir -p /var/run/wifi
cp /usr/local/bin/wifi/hostapd.conf /var/run/wifi/hostapd.conf
mknod -m 666 /dev/random c 1 8
mknod -m 666 /dev/urandom c 1 9
cd /usr/local/bin/wifi
./go_ap.sh
```

The monitor is now a Wi-Fi access point at `<AP-IP>` (`192.168.111.1`). On the
**PC**, join that access point (`<AP-SSID>`) and confirm you can reach it:

```sh
ping -c2 <AP-IP>
```

---

## Step 4 — Mount the monitor's writable storage

Everything for the SSH server lives on the monitor's writable **data** partition
(`mtd4`), which the normal boot mounts at `/mntfs`. The recovery shell does not
mount it automatically, so mount it by hand. Run on the **monitor**:

```sh
mknod /dev/ubi_ctrl c 10 61
cd /usr/local/bin
./ubiattach -m 4 -d 4 /dev/ubi_ctrl
```

Find the volume node number it created, make the node, and mount:

```sh
cat /sys/class/ubi/ubi4_0/dev # prints e.g. 250:1 (major:minor)
mknod /dev/ubi4_0 c 250 1 # use YOUR numbers if different
mount -t ubifs ubi4_0 /mntfs
ls /mntfs # should list: wifi checksum storage...
```

> If `cat /sys/class/ubi/ubi4_0/dev` prints different numbers, use those in the
> `mknod` line. The `ls /mntfs` listing confirms it mounted.

---

## Step 5 — Install the SSH server on the monitor (no flash write yet)

This copies files onto the data partition and tests the login. Nothing here
touches the boot — it is fully reversible.

**On the PC**, in the dropbear build folder, serve the binary and your key:

```sh
cd ~/dropbear-2025.89
cp ~/.ssh/id_ed25519.pub.
python3 -m http.server 8000
```

Leave that running. **On the monitor**, download and lay everything out:

```sh
mkdir -p /mntfs/dropbear
cd /mntfs/dropbear
wget http://<PC-IP-ON-AP>:8000/dropbearmulti
wget http://<PC-IP-ON-AP>:8000/id_ed25519.pub -O authorized_keys
chmod +x dropbearmulti
ln -sf dropbearmulti dropbear
ln -sf dropbearmulti dropbearkey
chmod 600 authorized_keys
./dropbearkey -t ed25519 -f /mntfs/dropbear/dropbear_ed25519_host_key
```

Create the start script. Paste the whole block as one piece:

```sh
cat > /mntfs/dropbear/start.sh <<'EOF'
#!/bin/sh
D=/mntfs/dropbear
[ -d /dev/pts ] || mkdir -p /dev/pts
mount | grep -q devpts || mount -t devpts devpts /dev/pts 2>/dev/null
[ -e /dev/ptmx ] || mknod /dev/ptmx c 5 2
chmod 666 /dev/ptmx 2>/dev/null
"$D/dropbearmulti" dropbear -E -s -p 22 -r "$D/dropbear_ed25519_host_key" -D "$D" -P /var/run/dropbear.pid &
EOF
chmod 755 /mntfs/dropbear/start.sh
sync
```

Now **go back to the PC and stop the HTTP server** (`Ctrl-C` in that terminal).

Test it — start the server on the monitor:

```sh
sh /mntfs/dropbear/start.sh
```

From the **PC**, log in (port 22, over the access point). Accept the key
fingerprint prompt the first time:

```sh
ssh -i ~/.ssh/id_ed25519 root@<AP-IP>
```

You should land at a root prompt (it may be blank — type `id`; it prints
`uid=0(root)`). **If this works, the hard part is done.** If it says "Permission
denied", your `authorized_keys` does not match your key — re-download it with the
`wget... -O authorized_keys` line above (do not paste it by hand).

---

## Step 6 — Make it start automatically (the one flash write)

This is the only step that writes to flash. It adds **one line** to the monitor's
boot script so the SSH server starts on every normal boot. Run on the **monitor**.

First, back up the original boot script to the data partition (your undo file):

```sh
cp /root/boot_app.sh /mntfs/dropbear/boot_app.sh.orig
```

Make the root filesystem writable, add the line with `sed`, flush, and lock it
back to read-only:

```sh
mount -o remount,rw /
sed -i '/^cmdapp &/i [ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &' /root/boot_app.sh
sync
mount -o remount,ro /
```

Verify the change is exactly one added line and nothing else:

```sh
diff /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh
```

The output must show only this one added line (a `>` line):

```
> [ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &
```

> If `diff` shows anything else, undo it now while you still have this shell:
> `mount -o remount,rw / && cp /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh && sync && mount -o remount,ro /`, then redo the `sed` line.

---

## Step 7 — Test the real thing

Power-cycle the monitor and let it boot **normally** — do **not** touch the
serial keyboard, do **not** interrupt U-Boot. Wait about a minute for it to boot
and join your home Wi-Fi.

On the **PC**, switch Wi-Fi back to your home network, find the monitor's address
`<LAN-IP>` in your router's device list (by its MAC), and log in:

```sh
ssh -i ~/.ssh/id_ed25519 root@<LAN-IP>
```

A root prompt = success. The SSH server now starts by itself on every boot, with
no serial cable and no cloud. Power-cycle once more and log in again to confirm
it is reliable.

> Reserve `<LAN-IP>` for the monitor in your router (static lease by MAC) so the
> address never changes. Then, optionally, add a shortcut on the PC — put this in
> `~/.ssh/config` and connect with just `ssh monitor`:
> ```
> Host monitor
> HostName <LAN-IP>
> User root
> IdentityFile ~/.ssh/id_ed25519
> ```

---

## Step 8 — If a boot ever fails (recovery)

A broken `boot_app.sh` does **not** brick the monitor and needs **no** reflash.
Boot the recovery root shell again (step 3: U-Boot → `init=/bin/sh`), mount the
data partition (step 4), and restore the original boot script:

```sh
mount -o remount,rw /
cp /mntfs/dropbear/boot_app.sh.orig /root/boot_app.sh
sync
mount -o remount,ro /
```

Power-cycle. The monitor is back to stock.

> On a *normal* boot the serial console is silent (the stock app holds it), so a
> failed auto-start can only be fixed from the `init=/bin/sh` recovery boot —
> which is exactly why step 5 proves the login works **before** step 6 wires it
> in. The full-flash backup from step 1 is the deeper fallback if the data
> partition itself is ever lost.

---

## Summary

| | |
|---|---|
| **Login** | `ssh -i ~/.ssh/id_ed25519 root@<LAN-IP>` (port 22, key only, no password) |
| **Server files** | `/mntfs/dropbear/` on the monitor (survives reboots) |
| **Auto-start** | one line in `/root/boot_app.sh`, backed up as `boot_app.sh.orig` |
| **Undo** | restore `boot_app.sh.orig` from the recovery boot (step 8) |
| **Why the odd build flags** | `-fno-strict-aliasing` (else the handshake fails) and `-march=armv6` (the CPU is ARM1176, not ARMv7) |
