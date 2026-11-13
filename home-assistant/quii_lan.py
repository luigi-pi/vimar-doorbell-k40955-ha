#!/usr/bin/env python3
"""quii_lan - minimal LAN video relay for the Vimar K40955 (QUII protocol, TCP 34567) -> raw H.264 on stdout.

Made for go2rtc's exec source, so HA needs no add-on and no extra port:
    streams:
      vimar_video:
        - exec:python3 /config/vimar/quii_lan.py <MONITOR_IP> /config/vimar/stream_credentials.json

Only the LAN preview with a saved key: no cloud, no key refresh, no door commands, no audio. Requires Python 3
and `cryptography` (both present in the Home Assistant core container). Credentials: quii-helper's
stream_credentials.json ({"stream_key": 32 chars, "auth_code": ...}).

Protocol reimplemented from quii-helper 0.4.0 (MIT, https://github.com/xaer981/quii-helper), LanTcpSession:
setup 0xA9 -> crypto_mode 2 (AES-256-CBC, fixed IV "0"*16 per piece, SHA256 trailer) -> live open (type 0x01,
body "adminapp2&&<sha256(auth)>\\0/mode=real&idc=1&ids=<stream>&ap=2\\0") -> media packets 0xA0-0xA3 carrying
CPacket frames (00 00 01 E0..EA, 20-byte header, H.264 Annex-B payload). Keep-alive every 10 s (type 0x00).
Viewing makes the monitor request the outdoor camera (ACB 40 01, T34): the first ~1 s is black.

usage: quii_lan.py <monitor ip> <stream_credentials.json> [--channel 1] [--stream 1] [--log FILE] [--notify URL]
  --log FILE: append messages and errors to FILE (go2rtc does not always show exec stderr)
  --notify URL: POST to URL (an HA webhook) once when the monitor sends its hang-up notice, i.e. when someone
                swipes the remote-connection page away on the indoor monitor (T57); the monitor closes the
                session 10 s later by itself (T46)
"""
import fcntl, hashlib, json, os, socket, struct, sys, tempfile, threading, time, urllib.request

Cipher = algorithms = modes = None         # imported in main() after the log is set up, so a missing module is logged

IV = b"0" * 16
MEDIA_TYPES = (0xA0, 0xA1, 0xA2, 0xA3)
PLAY_REPLY_TYPES = (0x01, 0x0C, 0x0D, 0x11, 0xAA)
KEEPALIVE_S = 10.0
MAX_PAYLOAD = 5 * 1024 * 1024
CAMERA_OFF_AVG = 40          # bytes per frame over 100 frames: camera off ~15, dark night ~200, day 300-600 (T56/T58)
CAMERA_OFF_RETRY_S = 60      # end a camera-off session at most once per minute per monitor (no reconnect loop)


def log(msg):
    sys.stderr.write(f"quii_lan: {msg}\n")
    sys.stderr.flush()


def aes(data, key, decrypt):
    c = Cipher(algorithms.AES(key), modes.CBC(IV))          # fresh fixed IV for every piece (native behaviour)
    x = c.decryptor() if decrypt else c.encryptor()
    return x.update(data) + x.finalize()


def packet(key, body, ptype, play_param=0, channel=0, stream_flag=0):
    raw_len = len(body)
    padded = (raw_len + 32 + 15) // 16 * 16                  # body + SHA256, padded to the AES block
    h = bytearray(32)
    h[0] = ptype
    h[1:9] = struct.pack("<Q", int(time.time()))
    h[9:11] = struct.pack("<H", padded)
    h[11:13] = struct.pack("<H", raw_len)
    h[13], h[14] = channel & 0xFF, channel >> 8
    h[15], h[16] = play_param, stream_flag
    trailer = hashlib.sha256(bytes(h) + body).digest()
    return aes(bytes(h), key, False) + aes((body + trailer).ljust(padded, b"\0"), key, False)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("monitor closed the connection")
        buf += chunk
    return bytes(buf)


class CFrames:
    """Reassemble CPacket frames across media payloads and return the H.264 bitstreams."""

    def __init__(self):
        self.buf = bytearray()
        self.total = None

    @staticmethod
    def starts(b, i):
        return i + 4 <= len(b) and b[i:i + 3] == b"\0\0\x01" and 0xE0 <= b[i + 3] <= 0xEA

    def feed(self, data):
        out, i = [], 0
        while i < len(data):
            if self.starts(data, i):
                self.buf, self.total = bytearray(), None       # a new frame header resets the current one
            elif not self.buf:
                j = data.find(b"\0\0\x01", i + 1)
                while j >= 0 and not self.starts(data, j):
                    j = data.find(b"\0\0\x01", j + 1)
                if j < 0:
                    break
                i = j
                continue
            if self.total is None:
                take = min(20 - len(self.buf), len(data) - i)
                self.buf += data[i:i + take]
                i += take
                if len(self.buf) < 20:
                    break
                self.total = int.from_bytes(self.buf[4:8], "little") + 20
                if self.total > MAX_PAYLOAD:
                    self.buf, self.total = bytearray(), None
                    continue
            take = min(self.total - len(self.buf), len(data) - i)
            self.buf += data[i:i + take]
            i += take
            if len(self.buf) < self.total:
                break
            frame_type = (self.buf[3] + 0x20) & 0xFF
            codec = self.buf[0x0E]
            if frame_type in (0, 1, 9, 10) and (codec == 1 or 0x10 <= codec <= 0x1F):   # H.264 video, not audio
                w = int.from_bytes(self.buf[0x10:0x12], "little")
                hgt = int.from_bytes(self.buf[0x12:0x14], "little")
                payload = bytes(self.buf[20:self.total])
                out.append((payload, frame_type in (1, 9), w, hgt))
            self.buf, self.total = bytearray(), None
        return out


def annexb(payload):
    """Start the frame at its first H.264 start code, as 00 00 00 01 (go2rtc detects raw H.264 by these bytes;
    the device payload may carry bytes before it, quii-helper also starts at this 'nal_offset')."""
    i = payload.find(b"\0\0\0\x01")
    j = payload.find(b"\0\0\x01")
    if i >= 0 and (j < 0 or i <= j):
        return payload[i:]
    if j >= 0:
        return b"\0" + payload[j:]
    return b""


def normalise_nri(nal):
    """The monitor writes SPS/PPS with nal_ref_idc 2 (header 0x47/0x48); go2rtc detects raw H.264 by
    00 00 00 01 67. For decoding only zero vs non-zero nal_ref_idc matters, so set SPS/PPS to 3 (0x67/0x68)."""
    b = bytearray(nal)
    i = 0
    while True:
        i = b.find(b"\0\0\x01", i)
        if i < 0 or i + 3 >= len(b):
            return bytes(b)
        t = b[i + 3] & 0x1F
        if t in (7, 8) and b[i + 3] & 0x60:
            b[i + 3] |= 0x60
        i += 3


LOCK_WAIT_S = 8.0
_sock = None


def camera_off_retry_allowed(host):
    """True at most once per CAMERA_OFF_RETRY_S per monitor (marker file next to the lock file)."""
    path = os.path.join(tempfile.gettempdir(), f"quii_lan_{host}.camoff")
    try:
        if time.time() - os.stat(path).st_mtime < CAMERA_OFF_RETRY_S:
            return False
    except OSError:
        pass
    open(path, "w").close()
    return True


def single_instance(host):
    """One live session per monitor at a time (T35: overlapping/abandoned sessions hung mainapp's live service).
    A second copy (e.g. go2rtc's probe and play starting together) waits for the first to finish."""
    path = os.path.join(tempfile.gettempdir(), f"quii_lan_{host}.lock")
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    end = time.monotonic() + LOCK_WAIT_S
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fd                                   # held until the process exits
        except BlockingIOError:
            if time.monotonic() > end:
                log("another session to this monitor is still running, giving up")
                sys.exit(1)
            time.sleep(0.2)


def notify(url):
    """POST once, in the background: the video must not wait for HA."""
    def post():
        try:
            urllib.request.urlopen(urllib.request.Request(url, data=b"", method="POST"), timeout=3).close()
            log("hang-up notice: webhook called")
        except Exception as e:
            log(f"hang-up notice: webhook failed: {e}")
    threading.Thread(target=post, daemon=True).start()


def run(host, cred_path, channel, stream, notify_url=None):
    global _sock
    cred = json.load(open(cred_path, encoding="utf-8"))
    key = cred["stream_key"].encode("utf-8")
    if len(key) != 32:
        sys.exit("quii_lan: stream_key must be 32 bytes")
    auth = cred["auth_code"]
    password = auth if len(auth) >= 64 else hashlib.sha256(auth.encode("utf-8")).hexdigest()

    single_instance(host)
    sock = _sock = socket.create_connection((host, 34567), timeout=15)
    sock.settimeout(15)
    sock.sendall(b"\xA9" + b"\0" * 31)                                  # setup
    setup = recv_exact(sock, 32)
    if setup[0] != 0xA9 or setup[9] != 0:
        sys.exit("quii_lan: setup rejected")
    if setup[10] != 2 or setup[11] != 1:
        sys.exit(f"quii_lan: unsupported crypto profile (mode {setup[10]}, sha {setup[11]})")
    path = f"/mode=real&idc={channel}&ids={stream}&ap=2"
    body = b"adminapp2&&" + password.encode() + b"\0" + path.encode() + b"\0"
    sock.sendall(packet(key, body, 0x01, play_param=1, channel=channel, stream_flag=stream - 1))
    log(f"connected to {host}, live open sent ({path})")

    out = sys.stdout.buffer
    frames = CFrames()
    playing, last_ka, sent = False, time.monotonic(), 0
    t0, win_bytes, win_keys = time.monotonic(), 0, 0
    while True:
        if playing and time.monotonic() - last_ka >= KEEPALIVE_S:
            sock.sendall(packet(key, b"", 0x00))
            last_ka = time.monotonic()
        h = aes(recv_exact(sock, 32), key, True)
        ptype = h[0]
        cmd_size = int.from_bytes(h[9:11], "little")
        is_media = ptype in MEDIA_TYPES
        size = int.from_bytes(h[11:15], "little") if is_media else cmd_size
        if not 0 < size <= MAX_PAYLOAD:
            sys.exit(f"quii_lan: bad packet length {size} (type {ptype:#x}) - wrong key?")
        raw = recv_exact(sock, size)
        if not is_media:
            if size % 16:
                sys.exit("quii_lan: bad command length - wrong key?")
            if ptype == 0xFE and h[13] == 1:            # transparent transfer type 1 = HangupAllPhone (T46)
                log("hang-up notice from the monitor (swipe on its remote-connection page)")
                if notify_url:
                    notify(notify_url)
                    notify_url = None                   # once per session
                continue
            if ptype in PLAY_REPLY_TYPES:
                if h[11] != 0:
                    sys.exit(f"quii_lan: play rejected (status {h[11]})")
                if not playing:
                    log("play accepted")
                playing = True
            continue
        if h[15] != 0:
            sys.exit("quii_lan: separate media encryption is not supported")
        offset = h[16] | (h[17] << 8)
        if cmd_size:                                    # an encrypted command prefix may precede the media
            raw = aes(raw[:cmd_size], key, True) + raw[cmd_size:]
        for payload, is_key, w, hgt in frames.feed(raw[offset:]):   # not 'key': that is the stream key
            if sent == 0 and not is_key:
                continue                                    # start the output at a keyframe (SPS/PPS/IDR)
            nal = normalise_nri(annexb(payload))
            if not nal:
                continue
            if sent == 0:
                log(f"first frame after {time.monotonic() - t0:.1f} s: {w}x{hgt}, {len(nal)} bytes, keyframe={is_key}, "
                    f"payload starts {payload[:8].hex(' ')}")
            out.write(nal)
            sent += 1
            win_bytes += len(nal)
            win_keys += is_key
            if sent % 100 == 0:   # ~5 s; average below ~200 bytes = uniform (black) picture
                log(f"{sent} frames, last 100: avg {win_bytes // 100} bytes, {win_keys} keyframes")
                if win_bytes // 100 < CAMERA_OFF_AVG and camera_off_retry_allowed(host):
                    # the outdoor camera went off under an open session (unanswered ring, monitor hang-up, ...):
                    # end it; if someone still watches, go2rtc reconnects and the new session's 40 01 switches
                    # the camera on again (T58)
                    sys.exit("camera off (black picture): ending the session")
                win_bytes, win_keys = 0, 0
        out.flush()


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    if "--log" in args:
        i = args.index("--log")
        if i + 1 < len(args):
            try:
                sys.stderr = open(args[i + 1], "a", buffering=1, encoding="utf-8")
            except OSError as e:
                log(f"cannot open log file {args[i + 1]}: {e}")
            del args[i:i + 2]
        else:
            log("--log needs a file name; logging to stderr")
            del args[i]
        log(f"start {time.strftime('%Y-%m-%d %H:%M:%S')} python {sys.version.split()[0]} at {sys.executable}")
    global Cipher, algorithms, modes
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except Exception as e:
        log(f"cannot import cryptography with {sys.executable} (python {sys.version.split()[0]}): {e}")
        sys.exit(1)
    opts = {"--channel": 1, "--stream": 1}
    for k in opts:
        if k in args:
            i = args.index(k)
            opts[k] = int(args[i + 1])
            del args[i:i + 2]
    notify_url = None
    if "--notify" in args:
        i = args.index("--notify")
        notify_url = args[i + 1]
        del args[i:i + 2]
    try:
        run(args[0], args[1], opts["--channel"], opts["--stream"], notify_url)
    except BrokenPipeError:                                     # go2rtc stopped reading: viewer gone
        log("stdout closed by go2rtc (viewer gone), exit")
    except KeyboardInterrupt:
        pass
    except (OSError, ConnectionError) as e:
        log(f"error: {e}")
        sys.exit(1)
    except SystemExit as e:
        if e.code not in (0, None):
            log(f"exit: {e.code}")
        raise
    except Exception as e:                                      # anything unexpected: keep it in the log
        log(f"error: {type(e).__name__}: {e}")
        raise
    finally:
        if _sock is not None:                                   # close the monitor session explicitly
            try:
                _sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            _sock.close()
        try:
            sys.stdout.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
