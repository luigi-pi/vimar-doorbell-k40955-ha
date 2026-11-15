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

## 4.4 Snapshot camera

Settings → Devices & services → **Add integration → Generic Camera**:

- **Still image URL:** `http://<HA_IP>:1984/api/frame.jpeg?src=vimar_k40955`
- **Stream source:** leave empty
- **Verify SSL:** off

Then open the new entity's settings and set its **Entity ID to `camera.vimar_door`** (the integration
first names it after the URL). This is the picture that appears in the ring notification.

**For the frozen picture** (the ring notification and the later *"No one answered"* both show the
caller's frame, saved at ring time), create a folder and allow Home Assistant to write the snapshot
into it. In the **Terminal & SSH** add-on:

```sh
mkdir -p /config/www/vimar
```

and add to `/config/configuration.yaml` (merge into an existing `homeassistant:` block if you have one):

```yaml
homeassistant:
  allowlist_external_dirs:
    - /config/www
```

Restart Home Assistant after this. Without it the notifications still work, but with no picture.

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

## 4.6 The door card and view

Copy `vimar-door-card.js` to `/config/www/` (create the folder `www` if it does not exist; a new
`www` folder needs one restart of Home Assistant).

Settings → Dashboards → ⋮ → **Resources → Add** → URL `/local/vimar-door-card.js`, type **JavaScript
module**. Then add a new dashboard view from [`door_view.yaml`](../home-assistant/door_view.yaml)
(⋮ → Edit in YAML, paste). It is laid out like the stock call screen: video, slide‑to‑open‑gate, round
Answer / Hang up.

> Keep this out of your dashboard's **first** view: opening it starts the video, which occupies the
> indoor monitor while it streams.

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
  caller's picture and the text **“No one answered”**, until you dismiss it or the next ring. To make
  it un‑swipeable, set `persistent: true` in the *"no one answered"* automation.

## Notes & limits

- Video is **H.264 Main, 352×288**. Chrome and the Android app play it over WebRTC (with microphone).
  Firefox's WebRTC is Baseline‑only, so for Firefox use a card in `mode: mse` (video + door audio, no
  microphone) and answer from Chrome or the app.
- Talk‑back needs the HTTPS internal URL (4.7). Without it you still get video and door audio.
- If the Lua patch was skipped in step 3 (other firmware), set `autostart_ringing: false` in the
  door view: starting the video during a ring would then answer the call.

## If something does not work

| Problem | Look at |
|---|---|
| No ring event, gate button does nothing | `vimar.py status` ([3.4](03-on-device.md#34-check)); MQTT user/password in `bridge.conf`; the broker's certificate changed → [Changing something later](03-on-device.md#changing-something-later) |
| No video, log says *wrong key* | fetch the stream key again (4.2); is the key lock active (3.4)? |
| Video works, no door audio / no talk‑back | `vimaraudio started` in the status (3.4); HTTPS internal URL and Autoplay (4.7) |
| The indoor monitor says *device occupied* | a phone or dashboard is showing the video; close it (see the note in 4.6) |
