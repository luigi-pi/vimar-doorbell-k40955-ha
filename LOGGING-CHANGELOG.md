# Central Vimar diagnostics change

Based on the uploaded `fix/camera-reset-reboot-sensors` source archive.

- Added `on-device/vimarlog.c`, a non-blocking Unix-datagram sender for shell supervisor messages.
- Extended `vimarbridge.c` to receive local log datagrams, publish JSON on `<base>/log`, and accept `<base>/cmd/log_level` over its existing MQTT command subscription.
- Added selected audio diagnostics, retaining the existing local RAM/stderr logs.
- Added an HA log-level selector, MQTT collector and local-only camera-log webhook automation.
- Added bounded, asynchronous webhook forwarding to `quii_lan.py`; frame statistics are DEBUG.
- Updated build, PC installer, shell installer, go2rtc example and documentation.

Device diagnostics are best-effort (QoS 0, non-retained and non-blocking); local RAM logs remain available when MQTT is unavailable. The HA `logger:` integration must be enabled with `vimar: debug` for INFO/DEBUG entries to appear in raw logs. No physical K40955 testing has been performed.
