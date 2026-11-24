# 3 — Install the on‑device components

Four small things are added to the monitor and start at every power‑on:

| Component | Job |
|---|---|
| **vimarbridge** | passes the door bus to Home Assistant over MQTT: ring, gate, answer, hang‑up, call state, reboot button, CPU / memory sensors |
| **vimaraudio** | carries the door microphone and speaker (listen + talk‑back) |
| **Lua patch** | lets a phone watch the camera *while the monitor is ringing*, without the monitor treating the viewer as an answer |
| **Key lock** | stops the monitor from changing its video key every 7 days (without it the video in Home Assistant goes dark after a week) |

Nothing in the monitor's firmware is changed. Everything is stored on its data partition
(`/mntfs/vimarbridge`); the Lua patch and the key lock work on **copies** of two files of your own
monitor and are undone by a switch file or by removing the folder.

**You need:** the login from [step 2](02-root-and-backup.md) (`ssh root@<MONITOR_IP>` works), the kit
folder with `dist`, and Home Assistant with an MQTT broker.

## 3.1 Prepare MQTT in Home Assistant

The bridge talks to the MQTT broker of your Home Assistant over an encrypted connection (port 8883).

1. **Broker.** If you do not have one: *Settings → Add‑ons → Add‑on store → Mosquitto broker →
   Install → Start*, then accept the **MQTT** integration that *Settings → Devices & services* offers.
2. **A user for the doorbell.** *Settings → People → Users → Add user*, for example `vimar`, with a
   password of its own. Note both.
3. **A certificate for the broker.** The encrypted port only works if the broker has a certificate.
   The bridge remembers ("pins") exactly this certificate, so it should not change. The simplest way
   is a certificate of its own, valid for 10 years. In the **Terminal & SSH** add‑on run:

   ```sh
   openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=mqtt" -keyout /ssl/mqtt-key.pem -out /ssl/mqtt-cert.pem
   ```

   Then in the Mosquitto add‑on, tab *Configuration*, set `certfile` to `mqtt-cert.pem` and `keyfile`
   to `mqtt-key.pem`, save and restart the add‑on.

   > Already using the broker's port 8883 with a Let's Encrypt certificate? That works too, but such
   > certificates are replaced every few weeks, usually with a new key. Each time the ring and the gate
   > stop working until you repeat [3.3](#33-install) (delete `broker.pem` first).

## 3.2 Your settings

In the kit folder, copy the example and fill in **three** values: the address of Home Assistant and
the MQTT user and password from 3.1.

**Windows**
```
copy on-device\bridge.conf.example bridge.conf
notepad bridge.conf
```
**Linux**
```sh
cp on-device/bridge.conf.example bridge.conf
nano bridge.conf
```

`host=` must be an address made of numbers (like `192.168.1.10`), not a name.
`bridge.conf` contains your MQTT password — keep it private.

## 3.3 Install

**Windows**
```
python tools\vimar.py install <MONITOR_IP>
```
**Linux**
```sh
python3 tools/vimar.py install <MONITOR_IP>
```

The command tells you what it does:

1. checks your settings and fetches the broker's certificate (saved as `broker.pem`);
2. copies the Lua file and the main program **from your monitor** into the folder `work`, and patches
   the copies (the main program twice: the key lock, and the key lock + high quality video that you
   switch on later in Home Assistant). Both tools only accept the files of firmware **V401R001B002**; with any other firmware
   they are skipped with a clear message and the rest is still installed;
3. copies everything to `/mntfs/vimarbridge` on the monitor, checking each file after the copy;
4. adds the start line to `/mntfs/dropbear/start.sh`.

Then restart the monitor (the intercom is unavailable for about three minutes):

```
ssh root@<MONITOR_IP> reboot
```

## 3.4 Check

After about three minutes:

**Windows**
```
python tools\vimar.py status <MONITOR_IP>
```
**Linux**
```sh
python3 tools/vimar.py status <MONITOR_IP>
```

You want to see, in the boot log, `Lua patch applied`, `OK: mainapp behind the bridge` and
`vimaraudio started`; three process numbers under *running*; and
`mainapp program: /mntfs/vimarbridge/app/mainapp` (the key lock).

Press the doorbell: the monitor rings as always. In Home Assistant (*Settings → Devices & services →
MQTT*) a new device with the ring event, the call state, the gate button, a **Reboot monitor**
button and four health sensors (**CPU usage**, **CPU load (1 min)**, **Memory used**,
**Memory available**) has appeared.

**Reboot monitor.** The button stops the monitor's program cleanly, switches the door camera off,
and reboots the monitor (about a minute, during which the doorbell is offline). It works in any call
state, so it is also the way out of a stuck one. If the reboot command does not take effect, the
monitor's program is started again by itself.

**Health sensors.** Published every 30 s, read from the monitor's `/proc`. *CPU usage* is the
average over the last 30 s. *Memory used / available* count the file cache as available; the monitor
has only about 90 MB, so watch *Memory available* while the high quality video runs. The sensors go
*unavailable* if the bridge stops reporting for 2.5 minutes.

**Door camera.** When the monitor's program is stopped from outside (the video quality switch, the
reboot button, or a crash) it cannot switch the door camera off itself. `boot.sh` now tells the bridge,
which then puts call audio and the camera off on the door bus and forgets the open video session.
Before this, the outdoor unit's camera light could stay on after a quality switch, and the bridge then
skipped the camera-off at the end of every later call.

**Updating an existing install:** run the install command of 3.3 again and reboot the monitor
(the new `boot.sh` and `vimarbridge` are copied; the old ones are kept as `*.prev`).

Continue with [step 4](04-home-assistant.md).

## Switching parts off, and undoing

Each line is one command on the PC, followed by `ssh root@<MONITOR_IP> reboot`.

| To do this | run |
|---|---|
| Switch **everything** of this step off (monitor behaves as delivered) | `ssh root@<MONITOR_IP> "touch /mntfs/vimarbridge/disable"` |
| Switch off only the Lua patch | `ssh root@<MONITOR_IP> "touch /mntfs/vimarbridge/disable-lua"` |
| Switch off only the audio helper | `ssh root@<MONITOR_IP> "touch /mntfs/vimarbridge/disable-audio"` |
| Switch off only the key lock (and HQ video) | `ssh root@<MONITOR_IP> "touch /mntfs/vimarbridge/disable-app"` |
| Switch a part on again | the same line with `rm` instead of `touch` |
| Remove this step completely | `ssh root@<MONITOR_IP> "touch /mntfs/vimarbridge/disable"`, reboot, then `ssh root@<MONITOR_IP> "rm -rf /mntfs/vimarbridge"` |

The installer keeps the previous version of each program next to the new one (`*.prev`).

If the bridge ever stops, `boot.sh` gives the door bus back to the monitor's own program and restarts
it, so the intercom keeps working on its own. If the key‑lock copy of the main program does not
start, `boot.sh` falls back to the original by itself; if the HQ video copy does not start, it goes
back to the key-lock copy and sets the quality to normal.

## Changing something later

Changed the MQTT password, the broker's certificate or the address of Home Assistant? Edit
`bridge.conf` (and delete `broker.pem` if the certificate changed), run the install command of 3.3
again and reboot the monitor.

## For developers

`on-device/install.sh` is the Linux shell equivalent of `vimar.py install` (with `verify` and
`rollback`). `ramtest.sh`, `swap_vb.sh` and `lua-patch/lua_b1.sh` are RAM‑only test tools used during
development; they are not needed for an installation. The programs in `dist` are built by
[`build.sh`](building.md).
