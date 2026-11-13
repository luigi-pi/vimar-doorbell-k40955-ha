# 3 — On‑device components

Three small programs run on the monitor and start at boot:

| Component | Job |
|---|---|
| **vimarbridge** | relays the door‑bus (UART) to/from MQTT: ring, gate, answer, hang‑up, call‑state sensor |
| **vimaraudio** | streams the door microphone and speaker over RTSP (listen + talk‑back) |
| **base.lua patch** | lets a phone watch the camera *while the monitor is ringing*, without the monitor “answering for the cloud” |

All three are started by `boot.sh`, which is launched from the SSH hook you added in step 1. Nothing is
written to the firmware; everything lives on the writable data partition (`/mntfs`) and the Lua patch is a
RAM bind‑mount undone by a reboot.

You build on the Linux PC with the ARM cross‑compiler from step 1, then copy the result to the monitor.

```sh
sudo apt install -y gcc-arm-linux-gnueabi
F="-O1 -march=armv6 -marm -mfloat-abi=soft -fno-strict-aliasing -fno-strict-overflow -mno-unaligned-access -static"
```

> These flags are mandatory (same as dropbear in step 1). `-fno-strict-aliasing` in particular is required
> or the TLS/crypto code miscompiles on this GCC/CPU combination.

Build the two binaries into a staging folder `dist/` (so the installer finds them next to `boot.sh`):

```sh
cd on-device
mkdir -p dist
```

## 3.1 Build vimaraudio

```sh
arm-linux-gnueabi-gcc $F -Wall -o dist/vimaraudio vimaraudio/src/vimaraudio.c
```

## 3.2 Build vimarbridge (needs BearSSL 0.6)

The bridge talks to your MQTT broker over TLS, pinning the broker's own certificate (no CA, no clock).

```sh
# BearSSL 0.6:  https://bearssl.org/bearssl-0.6.tar.gz
B=~/bearssl-0.6
mkdir -p "$B/build/obj"
( cd "$B" && for f in $(find src -name '*.c'); do \
      arm-linux-gnueabi-gcc $F -Iinc -Isrc -c "$f" -o "build/obj/$(echo $f | tr / _).o"; done \
  && arm-linux-gnueabi-ar rcs build/libbearssl.a build/obj/*.o )

arm-linux-gnueabi-gcc $F -I"$B/inc" -o dist/vimarbridge vimarbridge.c "$B/build/libbearssl.a"
file dist/vimarbridge    # must say: ARM, statically linked
```

## 3.3 Patch the monitor's UI (base.lua)

Copy the UI file **from your own monitor** and patch it on the PC (the patcher checks it matches firmware
**V401R001B002** and refuses anything else):

```sh
scp root@<MONITOR_IP>:/usr/local/app/luaprj/src/base/base.lua .
python3 on-device/lua-patch/make_patch.py base.lua base.lua.patched
```

The patch changes two UI functions so the monitor keeps ringing (and showing its own picture) while a phone
watches, instead of treating the viewer as a cloud answer.

## 3.4 Configure the MQTT link

The bridge needs your MQTT broker. Create `bridge.conf` and export the broker's TLS certificate as
`broker.pem`:

```ini
# bridge.conf   (key=value)
host=<HA_IP>
port=8883
user=<MQTT_USER>
pass=<MQTT_PASSWORD>
cert=/mntfs/vimarbridge/broker.pem
base=vimar/k40955
node=vimar_k40955
```

```sh
# broker.pem = the broker's server certificate (the one it presents on 8883)
openssl s_client -connect <HA_IP>:8883 -servername <HA_IP> </dev/null 2>/dev/null \
  | openssl x509 > broker.pem
```

> Use a dedicated MQTT user for the doorbell. Do not commit `bridge.conf` or `broker.pem` — they hold your
> broker password and are device‑specific.

## 3.5 Install and start at boot

Put the config, certificate and patched Lua on the monitor, then run the installer (it copies the bridge,
the audio helper and `boot.sh`, and records the HA address for the audio stream):

```sh
cp boot.sh dist/
ssh root@<MONITOR_IP> 'mkdir -p /mntfs/vimarbridge/lua'
scp bridge.conf broker.pem root@<MONITOR_IP>:/mntfs/vimarbridge/
scp base.lua.patched root@<MONITOR_IP>:/mntfs/vimarbridge/lua/base.lua

VIMAR_SSH=root@<MONITOR_IP> ./install.sh install dist <HA_IP>
```

Add one line to the SSH hook so `boot.sh` runs at every boot (right after dropbear), then reboot:

```sh
ssh root@<MONITOR_IP> "grep -q vimarbridge/boot.sh /mntfs/dropbear/start.sh || \
  echo '[ -x /mntfs/vimarbridge/boot.sh ] && /mntfs/vimarbridge/boot.sh &' >> /mntfs/dropbear/start.sh"
ssh root@<MONITOR_IP> reboot
```

## 3.6 Verify (after ~2 minutes)

```sh
ssh root@<MONITOR_IP> 'grep -E "Lua|OK:|vimaraudio" /tmp/vb/boot.log; pidof vimarbridge vimaraudio mainapp'
```

You want to see `Lua patch applied`, `OK: mainapp behind the bridge`, `vimaraudio started`, and three
process IDs. Press the doorbell: the monitor rings normally and (once Home Assistant is set up in step 4)
an MQTT `ring` event fires.

## Switches and rollback

| | |
|---|---|
| Disable everything at next boot | `touch /mntfs/vimarbridge/disable` |
| Disable only the Lua patch | `touch /mntfs/vimarbridge/disable-lua` |
| Disable only the audio helper | `touch /mntfs/vimarbridge/disable-audio` |
| Roll back the binaries | `./install.sh rollback` (restores the previous `*.prev`) |

If the bridge ever dies, `boot.sh` restores the stock UART and restarts the monitor app, so the intercom
keeps working on its own.
