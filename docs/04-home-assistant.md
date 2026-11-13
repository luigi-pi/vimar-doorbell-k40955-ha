# 4 — Home Assistant

Replace `<MONITOR_IP>` with the monitor's LAN address and `<HA_IP>` with your Home Assistant address
throughout.

## 4.1 Integrations you need

- **MQTT** — the same broker you put in `bridge.conf`. The bridge auto‑publishes discovery, so the ring
  event, the call‑state sensor and the gate/answer buttons appear by themselves once it connects.
- **go2rtc** — bundled with Home Assistant and with the WebRTC Camera add‑on; it runs the video relay.
- **WebRTC Camera** (AlexxIT) — install from **HACS**. It provides the `custom:webrtc-camera` element the
  door card uses, and the WebRTC transport for low‑latency video + microphone.
- **Generic Camera** — built in; used for the snapshot in the notification.

## 4.2 Get the stream key (one time)

The outdoor video is AES‑encrypted with a per‑device **stream key**. Fetch it once with
**[quii‑helper](https://github.com/xaer981/quii-helper)** (its cloud bootstrap). It writes a
`stream_credentials.json` (`stream_key`, `auth_code`). Copy that file to `/config/vimar/`.

After this the video path is fully local; you only need the helper again if the key ever changes.

> Keep `stream_credentials.json` out of any backup you share — it is your device key.

## 4.3 Video relay (go2rtc)

Copy the relay and wire up the stream:

```sh
# on the HA host
cp home-assistant/quii_lan.py /config/vimar/
```

Merge [`home-assistant/go2rtc.example.yaml`](../home-assistant/go2rtc.example.yaml) into your `go2rtc.yaml`
(fill in the two addresses), then restart go2rtc. The `--notify` webhook is what lets the **indoor monitor
take the call back** when you swipe it.

## 4.4 Snapshot camera

Settings → Devices & services → **Add integration → Generic Camera**:

- **Still image URL:** `http://<HA_IP>:1984/api/frame.jpeg?src=vimar_k40955`
- **Stream source:** leave empty
- **Verify SSL:** off

Then open the new entity's settings and set its **Entity ID to `camera.vimar_door`** (the integration
first names it after the URL). This is the picture that appears in the ring notification.

## 4.5 The package

Copy [`home-assistant/vimar_doorbell.yaml`](../home-assistant/vimar_doorbell.yaml) to
`/config/packages/vimar_doorbell.yaml` (enable packages in `configuration.yaml` if you have not:
`homeassistant: packages: !include_dir_named packages`).

Edit the **PHONES table** near the top — one line per phone/user:

```yaml
USER_ID_1: {notify: mobile_app_YOUR_PHONE, interactive: binary_sensor.YOUR_PHONE_interactive}
```

- `USER_ID_1` — Settings → People → Users (Advanced mode) → open the user → **ID**
- `mobile_app_YOUR_PHONE` — Developer tools → Actions → search `mobile_app`
- `binary_sensor.YOUR_PHONE_interactive` — enable **Interactive** in the companion app's sensors

Put the same Interactive sensors into the *“hang up when the phone screen goes off”* automation's trigger
list. Check the configuration and restart Home Assistant.

## 4.6 The door card and view

```sh
cp home-assistant/vimar-door-card.js /config/www/
```

Settings → Dashboards → ⋮ → **Resources → Add** → URL `/local/vimar-door-card.js`, type **JavaScript
module**. Then add a new dashboard view from [`home-assistant/door_view.yaml`](../home-assistant/door_view.yaml)
(⋮ → Edit in YAML, paste). It is laid out like the stock call screen: video, slide‑to‑open‑gate, round
Answer / Hang up.

> Keep this out of your dashboard's **first** view: opening it starts the video, which occupies the indoor
> monitor while it streams.

## 4.7 Companion app settings (Android)

On each phone, in the Home Assistant companion app:

| Setting | Why |
|---|---|
| **Autoplay videos** (Settings → Companion app) | door audio plays on answer without tapping the speaker |
| **Internal URL = your HTTPS URL** | the phone only grants the microphone (talk‑back) to a secure page |
| **Interactive** sensor enabled | screen‑off = hang up for the owner of the call |
| Notification channel **“doorbell”** importance/sound | set the ring sound once, on the first notification |

## 4.8 Try it

Block the monitor's internet at the router, then press the doorbell:

- a notification arrives at once, and a snapshot of the caller appears in it a few seconds later;
- tapping it opens the door view with live video (the monitor keeps ringing too);
- **Answer** gives you two‑way audio; **the slide** opens the gate; **Hang up** ends your side;
- answering or swiping on the indoor monitor takes the call back.

## Notes & limits

- Video is **H.264 Main, 352×288**. Chrome and the Android app play it over WebRTC (with microphone).
  Firefox's WebRTC is Baseline‑only, so for Firefox use a card in `mode: mse` (video + door audio, no
  microphone) and answer from Chrome or the app.
- Talk‑back needs the HTTPS internal URL (4.7). Without it you still get video and door audio.
