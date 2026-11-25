# 4 — Home Assistant

Replace `<MONITOR_IP>` with the monitor's address and `<HA_IP>` with your Home Assistant address
throughout.

**Copying files to Home Assistant.** Several steps say "copy a file to `/config/…`". The easiest ways:

- the **Samba share** add‑on: the folder appears in Windows Explorer as `\\<HA_IP>\config`
  (Linux file managers: `smb://<HA_IP>/config`);
- or the **File editor** / **Studio Code Server** add‑on, which can upload files.

The files to copy are in the kit's `home-assistant` folder.

## 4.1 Integrations you need

- **MQTT** — set up in [step 3.1](03-on-device.md#31-prepare-mqtt-in-home-assistant). The bridge
  announces itself, so the ring event, the call‑state sensor and the gate/answer buttons appear by
  themselves once it connects.
- **WebRTC Camera** (AlexxIT) — install from **HACS**. It provides the `custom:webrtc-camera` element
  the door card uses, the WebRTC transport for low‑latency video + microphone, and it runs **go2rtc**,
  the video relay, inside Home Assistant.
- **Generic Camera** — built in; used for the snapshot in the notification.

## 4.2 Get the stream key (one time)

The outdoor video is AES‑encrypted with a per‑device **stream key**. The monitor does not hand this
key out locally; it has to be fetched **once** from the vendor's cloud, with the account of the app
you used so far (vEye Pro or View Door). After that the video is fully local.

Do this **after step 3** (the key lock keeps the key from changing afterwards) and **before** you
block the monitor's internet access.

**Because the monitor is rooted, read the key straight off it** — no vendor account, no app
constants. In the kit folder, with the monitor reachable over SSH:

```
python3 tools/vimar.py getkey <MONITOR_IP>
```

(Windows: `python`.) This writes `stream_credentials.json` in the current folder. Copy it to
`/config/vimar/` in Home Assistant. Full details, the by‑hand version and a `--verify` check are in
[get-stream-key.md](get-stream-key.md).

<details>
<summary>Without root: quii‑helper (the cloud method)</summary>

If you did not root the monitor, use **[quii‑helper](https://github.com/xaer981/quii-helper)**
(Python 3.10 or newer):

1. Install it: `python -m pip install quii-helper` (Linux: `python3 -m pip install …`).
2. Follow *"Stream‑Key Bootstrap And Local Reuse"* in quii‑helper's `docs/setup.md`: create a small
   `.env` with your app account, the monitor's device ID and a handful of app constants, then run
   its bootstrap. The app constants have to be looked up as quii‑helper's guide describes.
3. It writes `stream_credentials.json` (`stream_key` + `auth_code`). Copy it to `/config/vimar/`.
</details>

> Keep `stream_credentials.json` out of any backup you share — it is your device key.

With the key lock from [step 3](03-on-device.md) the key no longer changes, so this is a one‑time
step. If the video ever stops with a *wrong key* error, read the key again the same way and replace
the file.

## 4.3 Video relay (go2rtc)

Copy `quii_lan.py` to `/config/vimar/` (next to `stream_credentials.json`).

Merge [`go2rtc.example.yaml`](../home-assistant/go2rtc.example.yaml) into `/config/go2rtc.yaml` (create
the file if it does not exist) and fill in the two addresses, then restart Home Assistant. The
`--notify` webhook is what lets the **indoor monitor take the call back** when you swipe it.

The exec line ends with `#killsignal=15#killtimeout=2`. Without it go2rtc stops the relay with SIGKILL when
the last viewer leaves, so the relay cannot log why it ended and closes its socket to the monitor only
because the kernel does it. With it go2rtc sends SIGTERM and `quii_lan.py` logs `session end: go2rtc stopped
the producer (SIGTERM): the last viewer left`, flushes its pending log messages (at most 1 s) and closes the
connection itself; SIGKILL follows after 2 s if it hangs. This changes what is logged, not how the monitor sees
the end of the session (a normal TCP close either way). To see go2rtc's own side add `log: exec: debug`
(`[exec] kill with signal=15`).

## 4.4 Snapshot camera

Settings → Devices & services → **Add integration → Generic Camera**:

- **Still image URL:** `http://<HA_IP>:1984/api/frame.jpeg?src=vimar_k40955`
- **Stream source:** leave empty
- **Verify SSL:** off

Then open the new entity's settings and set its **Entity ID to `camera.vimar_door`** (the integration
first names it after the URL). This is the picture that appears in the ring notification.

**For the saved picture** (the ring notification and the later *"not answered"* both show the
caller's frame, saved at ring time as `DDMMYY_hhmm.jpg` and kept for the number of weeks you set), create a folder and
allow Home Assistant to write into it. In the **Terminal & SSH** add-on:

```sh
mkdir -p /media/vimar
```

and add to `/config/configuration.yaml` (merge into an existing `homeassistant:` block if you have one):

```yaml
homeassistant:
  allowlist_external_dirs:
    - /media
```

Restart Home Assistant after this. Without it the notifications still work, but with no picture.

> The pictures are in Home Assistant's **media** folder, so they can only be opened by a logged-in
> user (the companion app logs in by itself to show them in the notification). They are also visible
> in **Media → Local media → vimar**. If `/media` does not exist on your install (Home Assistant
> Container/Core), use the folder set as `media_dirs: local:` instead.

## 4.5 The package

Copy [`vimar_doorbell.yaml`](../home-assistant/vimar_doorbell.yaml) to
`/config/packages/vimar_doorbell.yaml`. If you have not used packages before, add this to
`/config/configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Edit the **PHONES table** near the top of the file — one line per phone/user:

```yaml
USER_ID_1: {notify: mobile_app_YOUR_PHONE, interactive: binary_sensor.YOUR_PHONE_interactive}
```

- `USER_ID_1` — Settings → People → Users (Advanced mode) → open the user → **ID**
- `mobile_app_YOUR_PHONE` — Developer tools → Actions → search `mobile_app`
- `binary_sensor.YOUR_PHONE_interactive` — enable **Interactive** in the companion app's sensors

Put the same Interactive sensors into the *"hang up when the phone screen goes off"* automation's
trigger list. Check the configuration (Developer tools → YAML) and restart Home Assistant.

### Central Vimar logs

The package adds the **Vimar - log verbosity** selector. INFO is the default; WARNING and ERROR reduce noise, while DEBUG adds protocol, connection and camera-frame diagnostics. The monitor receives the setting over its existing MQTT connection. Camera-relay logs are sent by `quii_lan.py` to a local-only webhook.

Enable Home Assistant's logger integration in `/config/configuration.yaml` so INFO and DEBUG messages from the custom `vimar.*` loggers are not filtered out. Merge this into an existing `logger:` section rather than adding a second one:

```yaml
logger:
  logs:
    vimar: debug
```

The messages appear under **Settings → System → Logs → Home Assistant → Show raw logs**. The condensed view stores warnings and errors only; use **Show raw logs** for INFO and DEBUG. The selector's state is restored by Home Assistant and republished when Home Assistant starts. Keep DEBUG on only while diagnosing a problem.

**What each source logs** (all appear under the logger names below):

| Logger | Source | Lines worth knowing |
|---|---|---|
| `vimar.camera` | `quii_lan.py` (webhook) | `start …` (pid, parent pid, notify/log-webhook), `play accepted after X s`, `session end: <reason>; ran X s, N frames`, camera-off restart and its 60 s suppression, hang-up notice from the monitor, `waited X s for the previous session` |
| `vimar.card` | `vimar-door-card.js` (`system_log.write`) | `[user/app\|browser HH:MM:SS.mmm visible\|hidden] video opened: <why>`, `video closed: <why>`, `page hidden …`, `Hang up …`, call-state transitions (DEBUG) |
| `vimar.bridge` | `vimarbridge.c` (MQTT) | state changes, `mainapp: live session opened (40)` / `closed (41) after N s[, held back …]` |

`quii_lan.py` also writes the same lines, with millisecond time stamps and its pid, to its `--log` file, which is
the only copy when Home Assistant is down or slow (the first failed webhook post is noted there once; the
`session end` line carries the count of lost messages). Messages of one relay run share the `[pid]` prefix, so
a restart loop is easy to see.

`vimar.card` lines are sent only while the log selector allows their level (the same selector as above; a missing
selector means INFO). Set `log: false` in the card configuration to stop them.

The camera webhook is `vimar_k40955_log` and is restricted to local requests. Keep the go2rtc exec source on the same trusted LAN as Home Assistant; do not expose this webhook externally.

## 4.6 The door card and view

Copy `vimar-door-card.js` to `/config/www/` (create the folder `www` if it does not exist; a new
`www` folder needs one restart of Home Assistant).

Settings → Dashboards → ⋮ → **Resources → Add** → URL `/local/vimar-door-card.js`, type **JavaScript
module**. Then add a new dashboard view from [`door_view.yaml`](../home-assistant/door_view.yaml)
(⋮ → Edit in YAML, paste). It is laid out like the stock call screen: video, slide‑to‑open‑gate, round
Answer / Hang up.

> Keep this out of your dashboard's **first** view: opening it starts the video, which occupies the
> indoor monitor while it streams.

**Background and screen off.** When the page becomes hidden (app in the background, screen off, browser tab
hidden) the card closes the video and releases the microphone at once, and logs `video closed: page hidden`.
It does not use the stock card's 5 s timer: the Android app freezes page timers in the background
(`WebView.pauseTimers`), so a timer never fires there, while the `visibilitychange` event is still delivered.
go2rtc notices the closed WebRTC connection after about 5–6 s and then stops the relay (see 4.3). When the
page is visible again the card shows **Start video**; it does not reconnect by itself, because that would
open a new session on the indoor monitor each time you unlock the phone.

If **you** own an HA call (you answered from HA) the video is kept in the background by default, as before: the
screen-off automation (4.7, *Interactive* sensor) and the 120 s limit end the call. To close the video and end
the call as soon as the page is hidden, set `hidden_ends_call: true` in the card configuration.

## 4.7 Companion app settings (Android)

On each phone, in the Home Assistant companion app:

| Setting | Why |
|---|---|
| **Autoplay videos** (Settings → Companion app) | door audio plays on answer without tapping the speaker |
| **Internal URL = your HTTPS URL** | the phone only grants the microphone (talk‑back) to a secure page |
| **Interactive** sensor enabled | screen‑off = hang up for the owner of the call |
| Notification channel **"doorbell"** importance/sound | set the ring sound once, on the first notification |

## 4.8 Try it

Block the monitor's internet at the router, then press the doorbell:

- a notification arrives at once, and a snapshot of the caller appears in it a few seconds later;
- tapping it opens the door view with live video (the monitor keeps ringing too);
- **Answer** gives you two‑way audio; **the slide** opens the gate; **Hang up** ends your side;
- answering or swiping on the indoor monitor takes the call back;
- if **nobody answers** (not on a phone, in HA or on the monitor), the notification stays, with the
  caller's picture and the text **“hh:mm : not answered”** (the time of the ring), until you dismiss
  it or the next ring. To make it un‑swipeable, set `persistent: true` in the *"not answered"*
  automation. The pictures are in `/media/vimar/` (Media → Local media → vimar), named `DDMMYY_hhmm.jpg`; a nightly automation
  deletes those older than the number of weeks you set.

**Save button.** The floppy-disk button on the video saves the current frame in the same folder
(`/media/vimar/`, named `DDMMYY_hhmmss.jpg`) instead of downloading it to the phone.

**Settings pop-up (optional).** The cog under the video in [`door_view.yaml`](../home-assistant/door_view.yaml) opens a pop-up (needs the *browser_mod* integration from HACS) with: save a picture
at every ring (on/off), weeks to keep the pictures, a button to the saved pictures, minutes the ring
notification stays, and **High quality video**: 896×576 instead of 352×288 for the video and the
saved pictures. Switching takes about 15 s: the monitor's own program restarts (its screen too), an
open video reconnects by itself, and it never happens during a ring or a call (it waits until the
monitor is free). Keep the monitor's REC button on *off* or *picture* while HQ is on: video recording
uses the same picture stream.
Set the values once after the first restart: new numbers start at 1 and new switches start off.
With *save a picture* off nothing is stored; the ring notification shows a live frame and
*"not answered"* has no picture.

The same pop-up shows the monitor's **CPU usage, CPU load, memory used and memory available**
and has a **Reboot** button (asks for confirmation first). They are ordinary entities of the *Vimar
K40955* device (`sensor.vimar_k40955_cpu_usage`, `…_cpu_load_1_min`, `…_memory_used`,
`…_memory_available`, `button.vimar_k40955_reboot_monitor`), so you can also put them on any other
dashboard, graph them, or use them in automations (for example, press the reboot button when
*Memory available* stays very low). If your entity ids differ, check *Settings → Devices → Vimar
K40955* and adjust the ids at the end of the pop-up in `door_view.yaml`.

## Notes & limits

- Video is **H.264 Main, 352×288**. Chrome and the Android app play it over WebRTC (with microphone).
  Firefox's WebRTC is Baseline‑only, so for Firefox use a card in `mode: mse` (video + door audio, no
  microphone) and answer from Chrome or the app.
- Talk‑back needs the HTTPS internal URL (4.7). Without it you still get video and door audio.
- If the Lua patch was skipped in step 3 (other firmware), set `autostart_ringing: false` in the
  door view: starting the video during a ring would then answer the call.

## Open points

### Audio from the phone to the door does not always work

**Status:** open, not fixed. Listening at the door is not affected.

**Symptom.** After answering from Home Assistant the door audio plays, but what you say on the phone is not
heard at the door. The camera and ring are fine. Seen once so far (2026-11-25); whether it happens on every
answer or only some is not yet known.

**What the logs show** (`vimar.audio`, with the log selector on DEBUG), right after `relay: mainapp stood down, audio on`:

```
AI active: 8000 Hz, 16-bit, 320 samples/frame, 30 frames; AO available (not enabled until backchannel SETUP)
AO attr read: rate=16000 bits=16 mode=3 samples=640 frames=30; provisional slot=1280 bytes
WARNING AO slot header unexpected: seq=0 bits=16 mode=3 (attr bits=16 mode=3); disabling talkback
WARNING AO talkback disabled: cannot prime first AO slot
stats 2s: door_frames=50 ... ao_slots=0 bc_bytes=0 ... rx_pkts ch2=16 (talk-back expected on ch2)
```

- The microphone packets from the phone arrive at the helper (`ch2=16` per 2 s) but nothing is played
  (`ao_slots=0`, `bc_bytes=0`): the helper switched talk-back off for the rest of the call.
- The check that fails is the slot's sequence number (`seq == 0` in `ao_write_slot`, `on-device/vimaraudio/vimaraudio.c`).
  Bits and mode of the slot match the attributes, so the slot format is right.
- The slot was requested about 14 ms after `AO_ENABLE` (`AI active` line to the warning).

**Likely cause (unverified).** A race: the first output slot is read before the driver has set its sequence
number, and the helper does not retry. Not confirmed on the device.

**Not caused by** the background-video change (the audio helper was not modified by it); the new logging only
makes the failure visible.

**To do**
1. Find out whether it happens on every answer or intermittently (answer several times and compare the `vimar.audio` lines).
2. If it is the race: in `ao_enable_and_prime`, read the first slot again a few times (about 10 ms apart) before
   treating `seq == 0` as a fault, then log how many tries it took.
3. Rebuild `dist/vimaraudio`, install it (`vimar.py install`) and reboot the monitor, then repeat the test.
4. Check whether the fault flag is cleared for the next call, or whether talk-back stays off until the helper restarts.

## If something does not work

| Problem | Look at |
|---|---|
| No ring event, gate button does nothing | `vimar.py status` ([3.4](03-on-device.md#34-check)); MQTT user/password in `bridge.conf`; the broker's certificate changed → [Changing something later](03-on-device.md#changing-something-later) |
| No video, log says *wrong key* | fetch the stream key again (4.2); is the key lock active (3.4)? |
| Video works, no door audio / no talk‑back | `vimaraudio started` in the status (3.4); HTTPS internal URL and Autoplay (4.7) |
| The indoor monitor says *device occupied* | a phone or dashboard is showing the video; close it (see the note in 4.6) |
| The video keeps running with the phone locked | look for `vimar.card … page hidden` in the logs; none means the app did not deliver `visibilitychange` (note the app version); `hidden_ends_call` (4.6); the go2rtc line in 4.3 |
| The indoor monitor's screen is interrupted every few minutes | a stale viewer keeps the relay alive: every relay restart opens a new session on the monitor. Look at `session end:` lines and their `ran … s` |
