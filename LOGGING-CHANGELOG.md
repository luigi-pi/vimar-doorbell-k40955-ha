# Central Vimar diagnostics change

Based on the uploaded `fix/camera-reset-reboot-sensors` source archive.

- Added `on-device/vimarlog.c`, a non-blocking Unix-datagram sender for shell supervisor messages.
- Extended `vimarbridge.c` to receive local log datagrams, publish JSON on `<base>/log`, and accept `<base>/cmd/log_level` over its existing MQTT command subscription.
- Added selected audio diagnostics, retaining the existing local RAM/stderr logs.
- Added an HA log-level selector, MQTT collector and local-only camera-log webhook automation.
- Added bounded, asynchronous webhook forwarding to `quii_lan.py`; frame statistics are DEBUG.
- Updated build, PC installer, shell installer, go2rtc example and documentation.

Device diagnostics are best-effort (QoS 0, non-retained and non-blocking); local RAM logs remain available when MQTT is unavailable. The HA `logger:` integration must be enabled with `vimar: debug` for INFO/DEBUG entries to appear in raw logs. No physical K40955 testing has been performed.

## Background video stop and log clarity

- `vimar-door-card.js`: closes video and releases the microphone on `visibilitychange` → hidden, synchronously (the Android app freezes page timers, so the stock 5 s timer never runs there); an HA call owned by this phone is kept unless `hidden_ends_call: true`; no silent restart when the page is visible again. New options `hidden_ends_call`, `log`, `log_level`. Every open/close of the video and every decision of the card is logged as `vimar.card` through `system_log.write`, gated by the log selector.
- `quii_lan.py`: handles SIGTERM from go2rtc (`#killsignal=15#killtimeout=2` in the exec line) and logs `session end: <reason>; ran X s, N frames`; the reason covers viewer left, monitor closed, camera off, hang-up notice, wait for the previous session, errors. Lines carry a millisecond time and the pid; pending webhook messages are flushed (at most 1 s, within go2rtc's 2 s kill timeout) so the last lines reach HA; lost messages are counted and reported in the `session end` line. The camera-off restart is logged with its reason, and the 60 s suppression once per session.
- `vimarbridge.c`: INFO lines for the mainapp live session (`40` opened, `41` closed after N s, held back by an HA call, `41` without a session). Log only, no change in behaviour.
- `go2rtc.example.yaml`, `docs/04-home-assistant.md`: killsignal, logger table, background behaviour, troubleshooting rows.

Tested: card logic and DOM tests (Playwright, Chromium); `quii_lan.py` against a fake monitor and the real go2rtc 1.9.9 (viewer leaves, monitor closes, camera off, HA down, HA slow); a full chain (real `video-rtc.js` WebRTC in Chromium → go2rtc → relay → fake monitor) with page timers frozen as in the Android app. Not tested: the real K40955 / mainapp, the Android app itself (`visibilitychange` delivery with frozen timers is taken from the app source), the `vimarbridge.c` change on the device (syntax-checked only).
