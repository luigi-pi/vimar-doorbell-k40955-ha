# 2 — Root access and flash backup

In this step you get a permanent login on the monitor ("SSH root access") and, before anything is
changed, a complete backup of its memory chip ("flash").

The order matters, and the tools enforce it:

| | What happens | Written to the monitor |
|---|---|---|
| Steps 1–3 | Start the monitor in a temporary recovery mode and log in | **nothing** (everything is in RAM) |
| Step 4 | **Copy the whole flash to your PC** and verify the copy | **nothing** |
| Step 5 | Install the SSH server permanently | a folder on the data partition + **one line** in the boot script |
| Step 6 | Normal start, log in over your home network | — |

If you switch the monitor off at any point before step 5, it is exactly as it was before.
Step 5 can be undone at any time ([If something goes wrong](#if-something-goes-wrong)).

**Time:** about 30–45 minutes. **You need:** the serial connection from [step 1](01-serial-console.md).

## Before you start (PC still on your home network)

1. **The kit.** Download `vimar-k40955-kit.zip` from the
   [Releases page](https://github.com/luigi-pi/vimar-doorbell-k40955-ha/releases) and unzip it. It contains
   this guide, the tools and — in the folder `dist` — the ready‑made programs for the monitor.
   (No release yet, or you prefer to build them yourself? See [building.md](building.md).)
2. **A terminal in the kit folder.** Keep the serial terminal from step 1 open and open a **second**
   window:
   - **Windows:** open the kit folder in Explorer, click into the address bar, type `cmd`, press Enter.
   - **Linux:** open a terminal and `cd` into the kit folder.
3. **Your login key.** Create it once (press **Enter** at every question):

   ```
   ssh-keygen -t ed25519
   ```

   If it says the file *already exists*, answer `n` — you already have a key and it will be used.

In this guide **"on the PC"** means that second window, **"on the monitor"** means the serial terminal.

## Step 1 — Start the recovery mode

On the monitor (serial terminal): click into the window, then switch the monitor **off and on** and
**keep the Enter key pressed** until you see this prompt:

```
I.mx6.UL#
```

(If the monitor just starts normally, switch it off and try again.)

Now enter these three lines **one at a time**. Paste a line, compare it with the guide, press Enter:

```
setenv bootargs console=ttySGK0,115200 ubi.mtd=3 rootfstype=ubifs root=ubi0:rootfs ro init=/bin/sh mem=90M
```

```
snand read 0xC1000000 0x100000 0x200000
```

```
bootm
```

A lot of text scrolls by. After about half a minute it stops at a prompt that looks like this (press
Enter once if you do not see it; a message *"can't access tty; job control turned off"* is normal):

```
/ #
```

The monitor is now in recovery mode: its normal program is not running and it will not ring. The three
lines only changed a setting in memory. Never type `saveenv`.

## Step 2 — Switch on the monitor's own Wi‑Fi

On the monitor, enter these lines **one at a time** (paste, Enter, wait for the `/ #` prompt):

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

The monitor is now a Wi‑Fi access point. Show its name (and password, if it has one):

```sh
grep -E "^ssid|^wpa_passphrase" /var/run/wifi/hostapd.conf
```

**On the PC, connect to that Wi‑Fi.** The PC will say *"No internet"* — that is expected; you do not
need internet until step 6. Check that the monitor answers (stop with `Ctrl+C`):

```
ping 192.168.111.1
```

## Step 3 — Log in (temporary SSH server, in RAM)

On the PC, in the kit folder:

**Windows**
```
python tools\vimar.py serve
```
**Linux**
```sh
python3 tools/vimar.py serve
```

> Windows: if the firewall asks, allow Python on **private and public** networks.

The command prints **one line starting with `wget`**. Type (or paste) exactly that line on the monitor
and press Enter. It looks like this:

```sh
wget -O /tmp/r.sh http://192.168.111.100:8000/ram-ssh.sh && sh /tmp/r.sh 192.168.111.100
```

Wait until the monitor prints:

```
OK - temporary SSH server running (RAM only, nothing written to flash).
```

Then stop the command on the PC with `Ctrl+C` and try the login from the PC:

```
ssh root@192.168.111.1
```

The first time it asks *"Are you sure you want to continue connecting"* — type `yes`. You are logged
in when the prompt changes; type `id` (it answers `uid=0(root)`), then `exit`.

From here on you do not need to type on the serial terminal any more. Leave it connected.

## Step 4 — Back up the flash (do not skip)

On the PC:

**Windows**
```
python tools\vimar.py backup 192.168.111.1
```
**Linux**
```sh
python3 tools/vimar.py backup 192.168.111.1
```

It copies the monitor's complete flash (128 MB) into the folder `backup`, lets the monitor read the
flash a second time and compares both readings. It takes a few minutes. Continue **only** when it ends
with:

```
Backup complete and verified.
```

Copy the `backup` folder to a second safe place. It contains your monitor's keys and your Wi‑Fi
password: never upload or share it.

## Step 5 — Make the SSH server permanent

On the PC:

```
ssh root@192.168.111.1 "sh /tmp/ssh/make-permanent.sh"
```

The script copies the SSH server to the monitor's data partition and adds one line to the monitor's
boot script. It checks the boot script before and after; if anything is not exactly as expected it
changes nothing and says so. The result must end with:

```
OK - the SSH server is installed and will start at every power-on.
```

## Step 6 — Normal start

1. Switch the monitor **off and on** and **do not touch the keyboard** this time. (A key press during
   the countdown stops it at `I.mx6.UL#`; if that happens, type `boot` and press Enter.)
2. Connect the PC to your **home Wi‑Fi** again.
3. Wait a minute or two, then look up the monitor's address in your router (list of connected devices).
   Give it a **fixed address** there, so it never changes. This address is called `<MONITOR_IP>` in the
   rest of the guide.
4. Log in from the PC:

   ```
   ssh root@<MONITOR_IP>
   ```

   Answer `yes`, type `id` → `uid=0(root)`, then `exit`.

Done: the monitor works as a normal intercom again and you can log in over the network. Switch it off
and on once more and log in again to be sure. You can now remove the serial adapter (if you soldered
wires, leaving them in place makes a later recovery easier).

Continue with [step 3](03-on-device.md).

## If something goes wrong

**The monitor cannot be made unusable by this guide.** The only change to its firmware is the one
line, and the recovery mode of step 1 does not depend on it.

| Problem | What to do |
|---|---|
| Step 1: no `I.mx6.UL#` prompt | Your typing does not reach the monitor: see the table at the end of [step 1](01-serial-console.md). Keep Enter pressed *before* switching on. |
| Step 1: `bootm` prints nothing more | Switch the monitor off and on (it starts normally) and repeat step 1, checking every character of the three lines. |
| Step 2: a line answers *"File exists"* or *"Device or resource busy"* | You entered it twice — harmless, go on. |
| Step 3: `wget: can't connect` / nothing happens | The PC is not on the monitor's Wi‑Fi, `vimar.py serve` is not running, or the Windows firewall blocks Python (*Windows Security → Firewall → Allow an app → Python*: tick *Public*). |
| Step 3: `Permission denied (publickey)` | The key on the monitor is not the key of this PC. Run `vimar.py serve` and the `wget` line again. |
| Step 3: `REMOTE HOST IDENTIFICATION HAS CHANGED` | You repeated step 3 after a restart. Run `ssh-keygen -R 192.168.111.1` and log in again. |
| Step 4: the two readings differ | Run the backup again. Do not continue to step 5 without a verified backup. |
| Step 5 ends with `FAILED` | Nothing was changed (the message says what was unexpected). Please [open an issue](https://github.com/luigi-pi/vimar-doorbell-k40955-ha/issues) with that message. |
| Step 6: the monitor does not appear in the router | It is still a working intercom. Wait 3 minutes, check the router again. If it stays away, repeat steps 1–3 and look at it from there, or undo (below). |

### Undo

Normal case (the monitor is reachable on your network):

```
ssh root@<MONITOR_IP> "sh /mntfs/dropbear/make-permanent.sh undo"
```

If it is not reachable: repeat steps 1–3, then

```
ssh root@192.168.111.1 "sh /tmp/ssh/make-permanent.sh undo"
```

Either way the original boot script is put back (it was saved in step 5 and is compared byte by byte).
After the next power cycle the monitor runs exactly the delivered firmware.

### About the flash backup

Undoing this guide never needs the backup. It is the last‑resort insurance in case the flash content
is ever lost for another reason. Writing it back is a job for someone experienced (on this kit the
only way is the U‑Boot `snand write` command, which has not been tested) and is not covered here.

## What this step changed on the monitor

| | |
|---|---|
| `/mntfs/dropbear/` | SSH server, your public key, start script, the saved original boot script, the undo script |
| `/root/boot_app.sh` | one added line: `[ -x /mntfs/dropbear/start.sh ] && /mntfs/dropbear/start.sh &` |
| Login | `ssh root@<MONITOR_IP>` — key only, no password, port 22 |

Want to see or type every command yourself? The hand‑typed version of this step is in
[manual-root.md](manual-root.md).
