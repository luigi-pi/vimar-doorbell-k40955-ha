# Vimar Doorbell K40955 — Home Assistant integration

Run the **Vimar K40955** Wi‑Fi video door‑entry kit **fully locally in Home Assistant**, with the
monitor's internet access blocked. No cloud account, no app, no subscription — video, two‑way audio,
the ring and the gate all stay on your own network.

The K40955 is an OEM **Qualvision / Quvii** intercom. Out of the box it depends on a cloud service to
reach the outdoor camera and audio from a phone. This project removes that dependency: a small bridge
and relay run on the (rooted) monitor and hand the media straight to Home Assistant.

> This was reverse‑engineered and built on my own device, for local interoperability, with the help of
> AI (Anthropic Claude / Claude Code). Use at your own risk. Everything here is reversible and writes
> nothing permanent to the monitor's firmware except a single SSH‑enable line (see root guide).

## Tested with

| | |
|---|---|
| Doorbell | **Vimar K40955** (OEM Qualvision / Quvii) |
| Monitor firmware | **V401R001B002** |
| Monitor SoC | **Goke GK7101S** (ARM1176 / ARMv6, soft‑float) |
| Home Assistant | **2026.10** (Android companion app) |
| HA add‑ons / integrations | **go2rtc**, **WebRTC Camera** (AlexxIT, via HACS), **MQTT**, **Generic Camera** |

## What works

- **Live video** from the outdoor camera in Home Assistant (RTSP/WebRTC, 352×288).
- **Two‑way audio**: listen at the door and talk back from your phone.
- **Doorbell ring** as an instant HA notification, **with a snapshot** of the caller, and as a call‑state
  sensor you can automate on.
- **Open the gate / door** from Home Assistant.
- **Answer and hang up from your phone**, laid out like the indoor monitor's own call screen:
  - video shows on the phones **and** on the indoor monitor while it rings;
  - the first device to answer owns the call; the others show “answered by …”;
  - the **indoor monitor keeps priority** — answering or swiping on it takes the call back from a phone.
- **Everything above works with the monitor's internet blocked at the router.**
- The **indoor monitor keeps working as a normal intercom** throughout.

## How it fits together

```
 Outdoor station ──2‑wire bus──► K40955 monitor (rooted)
                                   │   ├─ vimarbridge   : door‑bus MCU  ⇄  MQTT  (ring, gate, answer, call state)
                                   │   ├─ vimaraudio    : door mic / speaker  ⇄  RTSP  (listen + talk)
                                   │   └─ base.lua patch : lets a phone watch while it rings
                                   ▼
                          Home Assistant (LAN)
                            ├─ go2rtc + quii_lan.py : video relay (on demand)
                            ├─ MQTT broker          : ring / gate / call state
                            ├─ door card + package  : the call UI on your phone
                            └─ Generic Camera       : the snapshot in the notification
```

## Install — four steps

Do them in order. Steps 1–3 are on the monitor (from a Linux PC on your LAN); step 4 is in Home Assistant.

1. **[Get root on the monitor](docs/01-root-access.md)** — a permanent, cloud‑free SSH login.
2. **[Back up the flash](docs/02-flash-backup.md)** — your safety net, do it before anything else.
3. **[Install the on‑device components](docs/03-on-device.md)** — the bridge, the audio helper and the
   Lua patch, all started at boot.
4. **[Set up Home Assistant](docs/04-home-assistant.md)** — the video relay, the card, the package,
   the companion‑app settings and the integrations.

## Repository layout

| Path | What it is |
|---|---|
| `on-device/vimarbridge.c` | door‑bus (UART) ⇄ MQTT bridge: ring, gate, remote answer, call state |
| `on-device/vimaraudio/` | door audio helper (listen + talk‑back over RTSP) |
| `on-device/lua-patch/` | tool that patches the monitor's UI so a phone can watch during a ring |
| `on-device/boot.sh`, `install.sh`, `swap_vb.sh`, `ramtest.sh` | boot hook and install / test helpers |
| `home-assistant/quii_lan.py` | lean LAN video relay for go2rtc (`exec:` source) |
| `home-assistant/vimar-door-card.js` | the door card for your dashboard |
| `home-assistant/vimar_doorbell.yaml` | HA package: notification, call ownership, monitor priority |
| `home-assistant/door_view.yaml` | the dashboard view that holds the card |

## Credits & licence

- Original code (bridge, audio helper, relay, card, Lua patch tool, Home Assistant config) is released
  under the **MIT licence** — see [LICENSE](LICENSE).
- The LAN video protocol is reimplemented from **[quii‑helper](https://github.com/xaer981/quii-helper)**
  (MIT).
- Audio‑path groundwork draws on the **WiBox** community
  ([segator/wibox-media](https://github.com/segator/wibox-media), GPL‑3.0, and duhow/wibox,
  Conclusio/wibox-audio) for a sibling Goke intercom.
- The Lua patch tool edits two functions of the vendor's own UI file on *your* device; no vendor code is
  distributed here.

Not affiliated with or endorsed by Vimar or Qualvision. “Vimar”, “VIEW” and “vEye” are trademarks of their
respective owners, used here only to identify the hardware.
