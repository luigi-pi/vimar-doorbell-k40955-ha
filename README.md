# Vimar Doorbell K40955 — Home Assistant integration

Run the **Vimar K40955** Wi‑Fi video door‑entry kit **fully locally in Home Assistant**, with the
monitor's internet access blocked. After a one‑time setup there is no cloud, no app and no
subscription — video, two‑way audio, the ring and the gate all stay on your own network.

The K40955 is an OEM **Qualvision / Quvii** intercom. Out of the box it depends on a cloud service to
reach the outdoor camera and audio from a phone. This project removes that dependency: a small bridge
and relay run on the (rooted) monitor and hand the media straight to Home Assistant.

> This was reverse‑engineered and built on my own device, for local interoperability, with the help of
> AI (Anthropic Claude / Claude Code). Use at your own risk. Everything here is reversible; the only
> change to the monitor's firmware is a single line that starts the SSH server
> (see [step 2](docs/02-root-and-backup.md)).

## What works

- **Live video** from the outdoor camera in Home Assistant (RTSP/WebRTC, 352×288).
- **Two‑way audio**: listen at the door and talk back from your phone. Talk‑back is not always working yet, see [open points](docs/04-home-assistant.md#open-points).
- **Doorbell ring** as an instant HA notification, **with a snapshot** of the caller, and as a call‑state
  sensor you can automate on.
- **Open the gate / door** from Home Assistant.
- **Answer and hang up from your phone**, laid out like the indoor monitor's own call screen:
  - video shows on the phones **and** on the indoor monitor while it rings;
  - the first device to answer owns the call; the others show “answered by …”;
  - the **indoor monitor keeps priority** — answering or swiping on it takes the call back from a phone.
- **Everything above works with the monitor's internet blocked at the router.**
- The **indoor monitor keeps working as a normal intercom** throughout.

## Is this for you?

You do **not** need to program or compile anything: the guide uses ready‑made programs and a small
helper tool, and every command is given for **Windows and Linux**. Be aware of what it does ask:

- **One hardware job.** You connect a USB‑serial adapter to three tiny pads on the back of the
  monitor, once. No parts are replaced; soldering is optional. ([Step 1](docs/01-serial-console.md))
- **Typing commands.** About a dozen on the monitor and a dozen on the PC, copied from the guide.
- **Time.** An afternoon for the first installation.
- **The video key** is read straight off the rooted monitor, once
  ([step 4.2](docs/04-home-assistant.md#42-get-the-stream-key-one-time)) — no vendor account needed.
- **Firmware.** Developed on monitor firmware **V401R001B002**. On another version the tools refuse the
  parts that depend on it instead of guessing.

## What you need

| | |
|---|---|
| The kit | Vimar K40955, installed and working, and (for the one‑time video key) the account of the app you use with it |
| For step 1 | a **3.3 V USB‑serial adapter** and a way to contact 1.27 mm pads — see [step 1](docs/01-serial-console.md) |
| A PC | Windows 10/11 or Linux with [Python 3](https://www.python.org/downloads/). `ssh` is built into both. |
| Home Assistant | with the Mosquitto (MQTT) add‑on, HACS, and HTTPS if you want talk‑back from the phone |

## Install — four steps

**Get the kit first:** download `vimar-k40955-kit.zip` from the
[Releases page](https://github.com/luigi-pi/vimar-doorbell-k40955-ha/releases) and unzip it. It contains
this guide, the tools and the ready‑made monitor programs (`dist`). To build the programs yourself
instead, see [docs/building.md](docs/building.md).

Then do the steps in order:

1. **[Connect to the serial console](docs/01-serial-console.md)** — three wires to the back of the
   monitor. Nothing is changed.
2. **[Root access and flash backup](docs/02-root-and-backup.md)** — a temporary login in RAM, then a
   verified **backup of the whole flash**, and only then the permanent SSH login.
3. **[Install the on‑device components](docs/03-on-device.md)** — the bridge, the audio helper, the diagnostic log sender, the
   Lua patch and the key lock. The sender is a small helper, not a background service.
4. **[Set up Home Assistant](docs/04-home-assistant.md)** — the video key, the video relay, the card,
   the package and the companion‑app settings.

### Windows and Linux

Commands are typed in a terminal window opened **in the kit folder**:

- **Windows:** open the folder in Explorer, click into the address bar, type `cmd`, press Enter.
  Python is called `python`.
- **Linux:** open a terminal, `cd` into the folder. Python is called `python3`.

Where a command differs between the two, the guide shows both. Commands for the *monitor* are the same
for everyone.

### What gets changed on the monitor, and how to undo it

| Change | Undo |
|---|---|
| One line in the boot script, to start the SSH server | `make-permanent.sh undo` — [step 2](docs/02-root-and-backup.md#undo) |
| Folders `/mntfs/dropbear` and `/mntfs/vimarbridge` on the data partition | switch files or delete the folder — [step 3](docs/03-on-device.md#switching-parts-off-and-undoing) |

The Lua patch and the key lock work on **copies** of two files of your own monitor; the originals in
the firmware are never modified, and no vendor code is distributed in this repository.

## How it fits together

```
 Outdoor station ──2‑wire bus──► K40955 monitor (rooted)
                                   │   ├─ vimarbridge   : door‑bus MCU  ⇄  MQTT  (ring, gate, answer, call state)
                                   │   ├─ vimaraudio    : door mic / speaker  ⇄  RTSP  (listen + talk)
                                   │   ├─ base.lua patch : lets a phone watch while it rings
                                   │   └─ key lock      : keeps the video key from changing every week
                                   ▼
                          Home Assistant (LAN)
                            ├─ go2rtc + quii_lan.py : video relay (on demand)
                            ├─ MQTT broker          : ring / gate / call state
                            ├─ door card + package  : the call UI on your phone
                            └─ Generic Camera       : the snapshot in the notification
```

## Tested with

| | |
|---|---|
| Doorbell | **Vimar K40955** (OEM Qualvision / Quvii) |
| Monitor firmware | **V401R001B002** |
| Monitor SoC | **Goke GK7101S** (ARM1176 / ARMv6, soft‑float) |
| Home Assistant | **2026.10** (Android companion app) |
| HA add‑ons / integrations | **go2rtc**, **WebRTC Camera** (AlexxIT, via HACS), **MQTT**, **Generic Camera** |

## Repository layout

| Path | What it is |
|---|---|
| `docs/` | the guide (steps 1–4), [get the stream key](docs/get-stream-key.md), [building](docs/building.md), the [manual method](docs/manual-root.md) |
| `tools/vimar.py` | PC helper for Windows / Linux: `serve`, `backup`, `install`, `getkey`, `status` |
| `build.sh` | builds the three monitor programs into `dist/` (also run by GitHub Actions for the Releases) |
| `on-device/ssh/` | scripts run on the monitor in step 2: temporary SSH server, make it permanent / undo |
| `on-device/vimarbridge.c` | door‑bus (UART) ⇄ MQTT bridge: ring, gate, remote answer, call state, reboot button, CPU / memory sensors |
| `on-device/vimaraudio/` | door audio helper (listen + talk‑back over RTSP) |
| `on-device/lua-patch/` | tool that patches a copy of the monitor's UI so a phone can watch during a ring |
| `on-device/keylock/` | tool that patches a copy of the monitor's main program so the video key stays valid |
| `on-device/getkey/` | `vgetkey`: reads the video stream key out of the running monitor (no cloud) |
| `on-device/boot.sh` | started at power‑on: puts the bridge, the patches and the audio helper in place |
| `on-device/install.sh`, `swap_vb.sh`, `ramtest.sh`, `lua-patch/lua_b1.sh` | development helpers (Linux shell); not needed for an installation |
| `home-assistant/quii_lan.py` | lean LAN video relay for go2rtc (`exec:` source) |
| `home-assistant/vimar-door-card.js` | the door card for your dashboard |
| `home-assistant/vimar_doorbell.yaml` | HA package: notification, call ownership, monitor priority |
| `home-assistant/door_view.yaml` | the dashboard view that holds the card |

Comments in the source refer to `T…` numbers and to `docs/08`, `docs/09`: those are entries of the
author's research log, which is not part of this repository.

## Licence

**MIT licensed** ([LICENSE](LICENSE)) — free to use, modify, share and sell; just keep the copyright and
licence notice. The quii‑helper (MIT) third‑party notice is kept in [LICENSE](LICENSE) too; the
software contained in the ready‑made programs is listed in [THIRD-PARTY.md](THIRD-PARTY.md).

The Lua patch and key lock tools edit copies of two files of the vendor's firmware on *your* device;
no vendor code is distributed in this repository.

## Credits — projects this work builds on

This project would not exist without the following. Full credit and thanks to their authors.

**Protocol & prior art** (what the relay and the approach are based on)

- [xaer981/quii‑helper](https://github.com/xaer981/quii-helper) — the QUVII LAN protocol and the one‑time
  cloud key bootstrap; `home-assistant/quii_lan.py` is a reimplementation of its LAN path (MIT).
- [fariborz0015/quii‑lan‑client](https://github.com/fariborz0015/quii-lan-client) — second reference for the
  QUVII framing and the listen/talk audio path (MIT).
- [MDalprato/VimarViewHomeKit](https://github.com/MDalprato/VimarViewHomeKit) — local‑video prior art on the
  sibling Vimar K42957 kit.
- [segator/wibox-media](https://github.com/segator/wibox-media) (GPL‑3.0), building on
  [duhow/wibox](https://github.com/duhow/wibox) and [Conclusio/wibox-audio](https://github.com/Conclusio/wibox-audio)
  — audio‑path groundwork (`gadi/adi_audio`, RTSP) for a sibling Goke GK710x intercom.
- [syntesys87/c300x-controller](https://github.com/syntesys87/c300x-controller) — the architecture model: a
  rooted indoor monitor serving its media locally.

**Software this project builds on or requires**

- [AlexxIT/go2rtc](https://github.com/AlexxIT/go2rtc) — runs the video relay and the WebRTC transport.
- [AlexxIT/WebRTC](https://github.com/AlexxIT/WebRTC) — the `custom:webrtc-camera` element the door card uses (HACS).
- [Dropbear SSH](https://matt.ucc.asn.au/dropbear/dropbear.html) — the small SSH server cross‑built for root access.
- [BearSSL](https://bearssl.org/) — the TLS stack linked into the MQTT bridge.
- [Home Assistant](https://www.home-assistant.io/) and its MQTT, Generic Camera and companion‑app components.

**Same hardware / related Home Assistant work** (reference, not required)

- [dpeddi/qualvision-tools](https://github.com/dpeddi/qualvision-tools) — earlier exploration of the Qualvision CGI.
- [ha-vimar/ha-vimar-intercom](https://github.com/ha-vimar/ha-vimar-intercom) — the closest existing HA project for Vimar.
- [fdaneluzzi/homeassistant-allo-wt7](https://github.com/fdaneluzzi/homeassistant-allo-wt7) — HA integration for a sibling Qualvision‑OEM monitor.
- [OpenIPC](https://openipc.org/cameras/vendors/goke) — Goke GK7101/7102 SoC support status.

Not affiliated with or endorsed by Vimar or Qualvision. “Vimar”, “VIEW” and “vEye” are trademarks of their
respective owners, used here only to identify the hardware.


## Central diagnostics

The Home Assistant package includes a Vimar log-level selector (INFO/WARNING/ERROR/DEBUG). On-device diagnostic messages are forwarded through the existing `vimarbridge` MQTT connection; `quii_lan.py` forwards camera diagnostics through a local-only HA webhook. View accepted Vimar messages in **Settings → System → Logs → Show raw logs**. See [docs/04-home-assistant.md](docs/04-home-assistant.md#central-vimar-logs). The monitor sender is best-effort and non-blocking; local RAM logs remain the fallback.
