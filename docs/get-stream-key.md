# Get the video stream key from the monitor (no cloud, no quii-helper)

The outdoor video is AES‑encrypted with a per‑device **stream key** (32 characters). Home Assistant
needs it, together with your **auth code** (the code on the monitor's back label), in a small file
`stream_credentials.json`.

There are two ways to obtain the key. If the monitor is already rooted (you did
[step 2](02-root-and-backup.md)), the method here reads it straight off the device and needs no
vendor account and no app constants. If you have **not** rooted the monitor, use
[quii‑helper instead](04-home-assistant.md#42-get-the-stream-key-one-time).

## How it works

The key is **not** stored on the monitor's disk. `mainapp` rebuilds it at every boot from the
secrets in `/mntfs/wifi/npc_nts_client_config.ini` and keeps it only in memory — as a parsed field,
the name `DeviceKey2` and the 32‑character value sitting next to each other (not as `name=value`
text). A tiny helper (`vgetkey`, built with the rest of the kit) finds the value next to that label
in the running `mainapp`. It opens the process memory **read‑only** and prints the one value; it
writes nothing to the monitor.

The `auth_code` is read the same way: mainapp holds it in memory as `app_token[<token>]` (you can see
it building the login string `adminapp2&&<token>&…`). So both values come off the device and you type
nothing. If a firmware stores the token elsewhere, pass your own with `--auth`.

## Do it

In the kit folder, with the monitor reachable over SSH (`ssh vimar` or `ssh root@<MONITOR_IP>`):

**Windows**
```
python tools\vimar.py getkey <MONITOR_IP>
```
**Linux**
```sh
python3 tools/vimar.py getkey <MONITOR_IP>
```

(If you use an SSH alias, pass it in place of `<MONITOR_IP>`, e.g. `getkey vimar`.)

It copies `vgetkey` to the monitor's `/tmp`, reads the stream key and the auth token, removes the
helper again, and writes `stream_credentials.json` in the current folder. Secrets are shown only
masked, so they do not sit in your terminal history. To override the auth token, add `--auth <token>`.

Then copy `stream_credentials.json` to Home Assistant at `/config/vimar/stream_credentials.json`.

## Check it against a key you already have

If you fetched a working key before (with quii‑helper), confirm this method returns the same key —
this proves the reader before you rely on it:

```
python3 tools/vimar.py getkey <MONITOR_IP> --verify path\to\old\stream_credentials.json --no-write
```

`VERIFY OK` means the key read from memory equals your known‑good key; a second line reports whether
the auth token matches too. `--no-write` means nothing is saved.

## By hand

If you prefer to run the reader yourself:

```sh
# on the monitor
PID=$(pidof mainapp)
/tmp/vgetkey -k $PID                      # the 32-char stream key (next to the DeviceKey2 label)
/tmp/vgetkey -b 'app_token[' ']' $PID     # the auth token (between these two markers)
/tmp/vgetkey -k -m $PID                   # masked: only the length and charset
```

Push `vgetkey` there first with `ssh <MONITOR_IP> "cat > /tmp/vgetkey && chmod +x /tmp/vgetkey" < dist/vgetkey`,
and remove it afterwards with `ssh <MONITOR_IP> "rm -f /tmp/vgetkey"`. Build `vgetkey` yourself with
[`build.sh`](building.md) if it is not in `dist/`.

## If the key is not found

`vgetkey -k` looks for a 32‑character value next to a `DeviceKey2` label in `mainapp`'s memory. If a
future firmware stores it differently, the command says so and `getkey` stops without writing.

- See how the key is stored: `/tmp/vgetkey -C '<a key you already have>' $PID` prints the bytes
  around it, so you can spot the label (pass it with `--label`).
- Confirm `mainapp` is running: `ssh <MONITOR_IP> "ps | grep -v grep | grep mainapp"`.
- If it still cannot be found, the quii‑helper method
  ([4.2](04-home-assistant.md#42-get-the-stream-key-one-time)) still works. Please also
  [open an issue](https://github.com/luigi-pi/vimar-doorbell-k40955-ha/issues) so the anchor can be updated.

## Privacy

The stream key and the auth code are device secrets. `stream_credentials.json` contains both — keep
it out of any backup or paste you share. `vgetkey -m` prints only the length and character type, so
you can describe a problem without revealing the key.
