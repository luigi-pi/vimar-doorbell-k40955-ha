#!/usr/bin/env python3
"""PC-side helper for the Vimar K40955 local integration.

The same commands work on Windows, Linux and macOS (on Windows type "python", elsewhere "python3").
Needs Python 3.8 or newer and the "ssh" program (built into Windows 10/11, Linux and macOS).

    python tools/vimar.py serve                   offer the SSH server and your key to the monitor (step 2)
    python tools/vimar.py backup  <MONITOR_IP>    copy the monitor's whole flash to this PC and verify it (step 2)
    python tools/vimar.py install <MONITOR_IP>    install the bridge, audio helper, Lua patch and key lock (step 3)
    python tools/vimar.py status  <MONITOR_IP>    show what is running on the monitor

Run it from the top folder of this kit. Add -h to a command for its options.
"""
import argparse
import functools
import hashlib
import http.server
import os
import ipaddress
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent
REMOTE = "/mntfs/vimarbridge"
SSH_OPTS = ["-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=15"]
BOOT_HOOK = "[ -x /mntfs/vimarbridge/boot.sh ] && /mntfs/vimarbridge/boot.sh &"


def die(msg):
    sys.exit("\nERROR: " + msg)


def step(msg):
    print("\n== " + msg, flush=True)


# ------------------------------------------------------------------------------------------- ssh
def target(host):
    if not re.fullmatch(r"[A-Za-z0-9_.@:\-]+", host):
        die("'%s' does not look like an address" % host)
    return host if "@" in host else "root@" + host


def ssh(host, command, stdin=None, capture=False, check=True):
    """Run one command on the monitor. Returns (exit code, output bytes if capture)."""
    if not shutil.which("ssh"):
        die("the 'ssh' program was not found. Windows: Settings > System > Optional features > OpenSSH Client.")
    r = subprocess.run(["ssh"] + SSH_OPTS + [target(host), command],
                       stdin=stdin if stdin is not None else subprocess.DEVNULL,
                       stdout=subprocess.PIPE if capture else None)
    if r.returncode == 255:
        die("cannot log in to %s.\n"
            "  - is the address right and the monitor switched on? (ping %s)\n"
            "  - 'Permission denied': the monitor has a different key than this PC (~/.ssh/id_ed25519)\n"
            "  - 'HOST IDENTIFICATION HAS CHANGED': run   ssh-keygen -R %s   and try again"
            % (host, host.split("@")[-1], host.split("@")[-1]))
    if check and r.returncode != 0:
        die("this command failed on the monitor (exit %d):\n  %s" % (r.returncode, command))
    return r.returncode, (r.stdout if capture else b"")


def upload(host, local, remote, mode):
    """Copy a local file to the monitor (written as <remote>.new first, then moved into place)."""
    with open(local, "rb") as f:
        ssh(host, "cat > %s.new && chmod %s %s.new && mv %s.new %s" % (remote, mode, remote, remote, remote), stdin=f)
    _, out = ssh(host, "sha256sum %s" % remote, capture=True)
    if out[:64].decode("ascii", "replace") != sha256_file(local):
        die("%s did not arrive intact on the monitor" % remote)


def download(host, remote, local):
    with open(local, "wb") as f:
        r = subprocess.run(["ssh"] + SSH_OPTS + [target(host), "cat %s" % remote], stdin=subprocess.DEVNULL, stdout=f)
    if r.returncode != 0:
        die("cannot read %s from the monitor" % remote)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def unix_text(path):
    """File content with Unix line ends and no byte-order mark (Windows editors add both)."""
    data = Path(path).read_bytes()
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.replace(b"\r\n", b"\n")


def is_arm_program(path):
    head = Path(path).read_bytes()[:20]
    return len(head) == 20 and head[:4] == b"\x7fELF" and head[18:20] == b"\x28\x00"


def need(path, hint):
    if not Path(path).is_file():
        die("%s is missing.\n  %s" % (path, hint))
    return Path(path)


DIST_HINT = "Unzip the ready-made programs from the Releases page into the 'dist' folder, or build them (docs/building.md)."


# ----------------------------------------------------------------------------------------- serve
def cmd_serve(a):
    dist = Path(a.dist)
    multi = need(dist / "dropbearmulti", DIST_HINT)
    if not is_arm_program(multi):
        die("%s is not a program for the monitor (ARM)" % multi)
    key = Path(a.key).expanduser()
    if not key.is_file():
        die("your SSH key %s does not exist yet. Create it first:\n  ssh-keygen -t ed25519\n"
            "(press Enter at every question)" % key)
    pub = key.read_text(encoding="utf-8", errors="replace").strip().splitlines()
    if len(pub) != 1 or not pub[0].startswith("ssh-"):
        die("%s is not a public SSH key (it must be the .pub file)" % key)

    stage = Path(tempfile.mkdtemp(prefix="vimar-serve-"))
    shutil.copy(multi, stage / "dropbearmulti")
    (stage / "authorized_keys").write_bytes(pub[0].encode() + b"\n")
    for name in ("start.sh", "ram-ssh.sh", "make-permanent.sh"):
        (stage / name).write_bytes(unix_text(need(TOP / "on-device" / "ssh" / name, "The kit is incomplete.")))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((a.monitor, 9))
        ip = s.getsockname()[0]
    except OSError:
        ip = "?"
    finally:
        s.close()
    if ip == "?" or ipaddress.ip_address(ip) not in ipaddress.ip_network(a.monitor + "/24", strict=False):
        print("\nWARNING: this PC does not seem to be connected to the monitor's Wi-Fi (its address here is %s,\n"
              "the monitor is %s). Join the monitor's Wi-Fi first, then start this command again." % (ip, a.monitor))
        ip = "<PC-IP>"

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(stage))
    try:
        httpd = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), handler)
    except OSError as e:
        die("cannot open port %d (%s). Is this command already running in another window?" % (a.port, e))
    extra = "" if a.port == 8000 else " %d" % a.port
    print("\nOffering the SSH server and your key (%s) to the monitor." % key.name)
    print("Windows: if the firewall asks, allow Python on PRIVATE and PUBLIC networks.\n")
    print("Now type this ONE line on the monitor (serial console), then press Enter:\n")
    print("    wget -O /tmp/r.sh http://%s:%d/ram-ssh.sh && sh /tmp/r.sh %s%s\n" % (ip, a.port, ip, extra))
    print("Each file the monitor fetches is listed below. When the monitor prints 'OK', press Ctrl+C here.\n", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        httpd.server_close()
        shutil.rmtree(stage, ignore_errors=True)


# ---------------------------------------------------------------------------------------- backup
def read_stream(host, command, sink=None, total=0):
    """Run a command on the monitor and hash (and optionally save) everything it prints."""
    p = subprocess.Popen(["ssh"] + SSH_OPTS + [target(host), command], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
    h, n, shown = hashlib.sha256(), 0, -1
    while True:
        chunk = p.stdout.read(1 << 18)
        if not chunk:
            break
        h.update(chunk)
        n += len(chunk)
        if sink:
            sink.write(chunk)
        if n >> 22 != shown:
            shown = n >> 22
            print("\r   %4d / %d MB" % (n >> 20, total >> 20), end="", flush=True)
    print("\r   %4d / %d MB" % (n >> 20, total >> 20))
    if p.wait() != 0:
        die("reading from the monitor failed (%s).\n"
            "  In the recovery shell the flash nodes are created by ram-ssh.sh - did it print 'OK'?" % command)
    return h.hexdigest(), n


def cmd_backup(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    step("reading the partition table")
    _, table = ssh(a.monitor, "cat /proc/mtd", capture=True)
    table = table.decode("ascii", "replace")
    print(table.strip())
    parts = [(int(m.group(1)), int(m.group(2), 16), m.group(3))
             for m in re.finditer(r'^mtd(\d+):\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+"([^"]*)"', table, re.M)]
    if not parts:
        die("no flash partitions found on the monitor")
    whole = [p for p in parts if p[2] == "all"]
    todo = whole if whole else parts          # the 'all' partition already contains every other one
    (out / "proc_mtd.txt").write_text(table, encoding="ascii", errors="replace")

    for num, size, name in todo:
        file = out / ("nand_all.bin" if name == "all" else "mtd%d_%s.bin" % (num, re.sub(r"\W", "_", name)))
        step("copying mtd%d '%s' (%d MB) to %s" % (num, name, size >> 20, file))
        part = file.with_name(file.name + ".part")
        with open(part, "wb") as f:
            digest, got = read_stream(a.monitor, "cat /dev/mtd%d" % num, f, size)
        if got != size:
            die("got %d bytes, expected %d - the copy is incomplete, try again" % (got, size))
        step("verifying (the monitor reads its flash a second time)")
        rc, remote = ssh(a.monitor, "sha256sum /dev/mtd%d" % num, capture=True, check=False)
        if rc == 0 and re.fullmatch(r"[0-9a-f]{64}", remote[:64].decode("ascii", "replace")):
            second = remote[:64].decode()
        else:
            second, _ = read_stream(a.monitor, "cat /dev/mtd%d" % num, None, size)
        if second != digest:
            die("the two readings differ - do not continue. Run the backup again; if it keeps failing, stop here.")
        if file.exists():
            file.unlink()
        part.rename(file)
        file.with_name(file.name + ".sha256").write_text("%s  %s\n" % (digest, file.name), encoding="ascii")
        print("OK  %s  sha256 %s" % (file, digest))

    print("\nBackup complete and verified. Copy the folder '%s' to a second safe place (USB stick, NAS)." % out)
    print("It contains your monitor's keys and Wi-Fi password: never upload or share it.")


# --------------------------------------------------------------------------------------- install
def read_conf(path):
    conf = {}
    for line in unix_text(path).decode("utf-8", "replace").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    return conf


def patch_tool(script, src, dst, *opts):
    """Run one of the patch tools of this kit. Returns (ok, message)."""
    r = subprocess.run([sys.executable, "-I", str(script), *opts, str(src), str(dst)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0, r.stdout.decode("utf-8", "replace").strip()


def cmd_install(a):
    host = a.monitor
    dist = Path(a.dist)
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)

    step("checking the files on this PC")
    for name in ("vimarbridge", "vimaraudio", "vimarlog"):
        if not is_arm_program(need(dist / name, DIST_HINT)):
            die("%s is not a program for the monitor (ARM)" % (dist / name))
    boot = need(TOP / "on-device" / "boot.sh", "The kit is incomplete.")
    conf_path = need(a.conf, "Copy on-device/bridge.conf.example to bridge.conf and fill in your values (docs/03-on-device.md).")
    conf = read_conf(conf_path)
    for k in ("host", "user", "pass"):
        if not conf.get(k) or (conf[k].startswith("<") and conf[k].endswith(">")):
            die("%s: fill in '%s='" % (conf_path, k))
    try:
        ipaddress.IPv4Address(conf["host"])
    except ValueError:
        die("%s: host= must be an IP address like 192.168.1.10 (names do not work on the monitor)" % conf_path)
    if conf.get("cert") != REMOTE + "/broker.pem":
        die("%s: keep the line  cert=%s/broker.pem" % (conf_path, REMOTE))
    port = int(conf.get("port", "8883"))
    ha_ip = a.ha_ip or conf["host"]
    try:
        ipaddress.IPv4Address(ha_ip)
    except ValueError:
        die("--ha-ip must be an IP address")
    print("MQTT broker %s:%d, user %s; Home Assistant (go2rtc) at %s" % (conf["host"], port, conf["user"], ha_ip))

    pem = Path(a.cert)
    if not pem.is_file():
        step("fetching the broker's certificate from %s:%d" % (conf["host"], port))
        try:
            socket.setdefaulttimeout(10)
            text = ssl.get_server_certificate((conf["host"], port))
        except Exception as e:
            die("could not get the certificate (%s).\n  Is the MQTT broker reachable with TLS on port %d? See docs/04-home-assistant.md (4.1)."
                % (e, port))
        pem.write_bytes(text.replace("\r\n", "\n").encode("ascii"))
        print("saved as %s" % pem)
    if b"BEGIN CERTIFICATE" not in pem.read_bytes():
        die("%s is not a certificate (PEM)" % pem)

    step("checking the monitor")
    rc, _ = ssh(host, "[ -x /mntfs/dropbear/start.sh ]", check=False)
    if rc != 0:
        die("the permanent SSH server is not installed on this monitor yet (docs/02-root-and-backup.md)")

    lua_file = keylock_file = hq_file = None
    if not a.no_lua:
        step("Lua patch: video on the phone while the monitor rings")
        tool = TOP / "on-device" / "lua-patch" / "make_patch.py"
        orig_md5, patched_md5 = re.search(r'ORIG, PATCHED = "([0-9a-f]{32})", "([0-9a-f]{32})"', tool.read_text()).groups()
        src = work / "base.lua"
        download(host, "/usr/local/app/luaprj/src/base/base.lua", src)
        if hashlib.md5(src.read_bytes()).hexdigest() == patched_md5:
            print("already installed and active on this monitor - kept")
        else:
            ok, msg = patch_tool(tool, src, work / "base.lua.patched")
            if ok:
                lua_file = work / "base.lua.patched"
                print("patched (%s)" % msg)
            else:
                print("SKIPPED: your monitor's base.lua is not the analysed one (firmware V401R001B002): %s\n"
                      "         Everything else works; only 'video during the ring' is not available.\n"
                      "         In home-assistant/door_view.yaml set  autostart_ringing: false" % msg)
    if not a.no_keylock:
        step("key lock: keeps the video key from changing every 7 days")
        src = work / "mainapp"
        download(host, "/usr/local/app/mainapp", src)
        ok, msg = patch_tool(TOP / "on-device" / "keylock" / "make_keylock.py", src, work / "mainapp.keylock")
        if ok:
            keylock_file = work / "mainapp.keylock"
            print("patched (%s)" % msg)
            ok, msg = patch_tool(TOP / "on-device" / "keylock" / "make_keylock.py", src, work / "mainapp.hq", "--hq")
            if ok:
                hq_file = work / "mainapp.hq"
                print("HQ video build (%s), switched on from Home Assistant" % msg)
        else:
            print("SKIPPED: your monitor's mainapp is not the analysed one (firmware V401R001B002): %s\n"
                  "         WARNING: without the key lock the video stops after at most 7 days, until you\n"
                  "         fetch a new stream key (docs/04-home-assistant.md, 4.2)." % msg)

    step("copying to the monitor")
    ssh(host, "mkdir -p %s/lua" % REMOTE)
    conf_clean = work / "bridge.conf.upload"
    conf_clean.write_bytes(unix_text(conf_path))
    upload(host, conf_clean, REMOTE + "/bridge.conf", "600")
    conf_clean.unlink()
    pem_clean = work / "broker.pem.upload"
    pem_clean.write_bytes(unix_text(pem))
    upload(host, pem_clean, REMOTE + "/broker.pem", "644")
    pem_clean.unlink()
    print("bridge.conf, broker.pem")
    # keep the previous programs as *.prev (roll back: see docs/03-on-device.md)
    ssh(host, "cd %s && for f in vimarbridge vimaraudio vimarlog boot.sh media.conf; do [ -e $f ] && cp -p $f $f.prev; done; true" % REMOTE)
    upload(host, dist / "vimarbridge", REMOTE + "/vimarbridge", "755")
    upload(host, dist / "vimaraudio", REMOTE + "/vimaraudio", "755")
    upload(host, dist / "vimarlog", REMOTE + "/vimarlog", "755")
    boot_clean = work / "boot.sh.upload"
    boot_clean.write_bytes(unix_text(boot))
    upload(host, boot_clean, REMOTE + "/boot.sh", "755")
    boot_clean.unlink()
    print("vimarbridge, vimaraudio, vimarlog, boot.sh")
    ssh(host, "cd %s && printf 'ha_ip=%s\\n' > media.conf && chmod 644 media.conf && rm -f disable disable-audio" % (REMOTE, ha_ip))
    if lua_file:
        upload(host, lua_file, REMOTE + "/lua/base.lua", "644")
        print("lua/base.lua")
    if keylock_file:
        _, cur = ssh(host, "sha256sum %s/app/mainapp 2>/dev/null; true" % REMOTE, capture=True)
        if cur[:64].decode("ascii", "replace") == sha256_file(keylock_file):
            print("app/mainapp (key lock) already installed")
        else:
            ssh(host, "rm -rf %s/app.new && mkdir -p %s/app.new" % (REMOTE, REMOTE))
            upload(host, keylock_file, REMOTE + "/app.new/mainapp", "755")
            ssh(host, "rm -rf %s/app.prev; [ -d %s/app ] && mv %s/app %s/app.prev; mv %s/app.new %s/app"
                % (REMOTE, REMOTE, REMOTE, REMOTE, REMOTE, REMOTE))
            print("app/mainapp (key lock)")
    if hq_file:
        _, cur = ssh(host, "sha256sum %s/app-hq/mainapp 2>/dev/null; true" % REMOTE, capture=True)
        if cur[:64].decode("ascii", "replace") == sha256_file(hq_file):
            print("app-hq/mainapp (HQ video) already installed")
        else:
            ssh(host, "rm -rf %s/app-hq.new && mkdir -p %s/app-hq.new" % (REMOTE, REMOTE))
            upload(host, hq_file, REMOTE + "/app-hq.new/mainapp", "755")
            ssh(host, "rm -rf %s/app-hq; mv %s/app-hq.new %s/app-hq" % (REMOTE, REMOTE, REMOTE))
            print("app-hq/mainapp (HQ video, off until switched on in Home Assistant)")
    ssh(host, "grep -q vimarbridge/boot.sh /mntfs/dropbear/start.sh || echo '%s' >> /mntfs/dropbear/start.sh; sync" % BOOT_HOOK)
    _, hook = ssh(host, "grep -c vimarbridge/boot.sh /mntfs/dropbear/start.sh", capture=True)
    if hook.strip() != b"1":
        die("the start line was not added to /mntfs/dropbear/start.sh")
    print("start at boot: on")

    if a.reboot:
        step("restarting the monitor")
        ssh(host, "reboot", check=False)
        print("Wait about 3 minutes, then:   %s tools/vimar.py status %s" % (Path(sys.executable).stem, host))
    else:
        print("\nInstalled. Restart the monitor to start everything:")
        print("    ssh %s reboot" % target(host))
        print("Wait about 3 minutes, then check:")
        print("    %s tools/vimar.py status %s" % (Path(sys.executable).stem, host))



# ---------------------------------------------------------------------------------------- getkey


def valid_key(k):
    return len(k) == 32 and re.fullmatch(r"[0-9A-Za-z]{32}", k) is not None


def write_credentials(out, stream_key, auth_code):
    import json
    data = {"stream_key": stream_key, "auth_code": auth_code}
    tmp = out + ".new"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, out)
    try:
        os.chmod(out, 0o600)
    except OSError:
        pass


def cmd_getkey(a):
    import json
    host = a.monitor
    vg = need(Path(a.dist) / "vgetkey", DIST_HINT)
    if not is_arm_program(vg):
        die("%s is not a program for the monitor (ARM)" % vg)

    step("finding mainapp on the monitor")
    _, out = ssh(host, "pidof mainapp", capture=True)
    pids = out.decode("ascii", "replace").split()
    if not pids:
        die("mainapp is not running. Wait a minute after boot, then try again.")

    step("copying the reader to the monitor (/tmp, removed afterwards)")
    upload(host, vg, "/tmp/vgetkey", "755")

    step("reading the stream key and auth token from mainapp's memory")
    key, auth_mem = "", ""
    for pid in pids:
        rc, res = ssh(host, "/tmp/vgetkey -k -l %s %s" % (a.label, pid), capture=True, check=False)
        good = sorted({ln.strip() for ln in res.decode("ascii", "replace").splitlines() if valid_key(ln.strip())})
        if len(good) > 1:
            ssh(host, "rm -f /tmp/vgetkey", check=False)
            die("found %d different 32-char values near the %r label; cannot choose.\n"
                "  Look by hand (docs/get-stream-key.md) and pass the right one with --key." % (len(good), a.label))
        if good:
            key = good[0]
            # the auth token sits in memory as app_token[<token>]
            _, ares = ssh(host, "/tmp/vgetkey -b '%s' '%s' %s" % (a.auth_open, a.auth_close, pid), capture=True, check=False)
            toks = sorted({ln.strip() for ln in ares.decode("ascii", "replace").splitlines()
                           if re.fullmatch(r"[0-9A-Za-z]{16,128}", ln.strip())})
            if len(toks) == 1:
                auth_mem = toks[0]
            break
    ssh(host, "rm -f /tmp/vgetkey", check=False)
    if a.key:                       # user supplied it explicitly -> trust that
        key = a.key
    if not key:
        die("could not find the stream key next to a %r label in mainapp.\n"
            "  See docs/get-stream-key.md to look by hand, or pass it with --key." % a.label)
    masked = key[:4] + "..." + key[-4:]
    print("stream key found (%s, 32 chars)" % masked)

    # --- verify against a known-good file, if asked ---
    if a.verify:
        try:
            kj = json.load(open(a.verify, encoding="utf-8"))
        except (ValueError, OSError) as e:
            die("cannot read %s: %s" % (a.verify, e))
        if kj.get("stream_key") != key:
            die("VERIFY FAILED: the key from memory does NOT match %s.\n"
                "  The label may be wrong, or that file is stale. Nothing written." % a.verify)
        print("VERIFY OK: the key from memory matches %s" % a.verify)
        if auth_mem and kj.get("auth_code"):
            print("VERIFY %s: the auth token from memory %s the one in %s"
                  % (("OK", "matches", a.verify) if auth_mem == kj["auth_code"]
                     else ("DIFF", "differs from", a.verify)))

    if a.no_write:
        print("(--no-write: nothing saved)")
        return

    # --- auth_code (only needed to write the file): --auth > --from > memory > existing file > ask ---
    auth = a.auth
    if not auth and a.from_file and Path(a.from_file).exists():
        try:
            auth = json.load(open(a.from_file, encoding="utf-8")).get("auth_code")
            if auth:
                print("auth_code taken from %s" % a.from_file)
        except (ValueError, OSError):
            pass
    if not auth and auth_mem:
        auth = auth_mem
        print("auth_code read from the monitor")
    if not auth and Path(a.out).exists():
        try:
            auth = json.load(open(a.out, encoding="utf-8")).get("auth_code")
        except (ValueError, OSError):
            pass
    if not auth:
        auth = input("Enter the auth code: ").strip()
    if not auth:
        die("no auth code found or given (use --auth, or --from an existing stream_credentials.json)")
    write_credentials(a.out, key, auth)
    print("\nwrote %s  (stream_key %s, auth_code set)" % (a.out, masked))
    print("Copy it to Home Assistant at /config/vimar/stream_credentials.json (docs/04-home-assistant.md, 4.2).")


# ---------------------------------------------------------------------------------------- status
def cmd_status(a):
    ssh(a.monitor,
        "echo \"uptime: $(cut -d. -f1 /proc/uptime) s\"; echo '--- boot log (last lines)'; tail -n 25 /tmp/vb/boot.log 2>/dev/null; "
        "echo '--- running'; echo \"vimarbridge: $(pidof vimarbridge)  vimaraudio: $(pidof vimaraudio)  mainapp: $(pidof mainapp)\"; "
        "echo \"mainapp program: $(ps | grep -v -e grep -e interDebug | grep -o '/[^ ]*/mainapp' | head -n 1)\"; "
        "echo '--- switches'; ls %s | grep -E '^disable' || echo 'none (everything enabled)'" % REMOTE)
    print("\nGood: 'OK: mainapp behind the bridge', 'vimaraudio started', three process numbers,\n"
          "      and (with Lua patch / key lock) 'Lua patch applied' and mainapp program = %s/app/mainapp" % REMOTE)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd")

    s = sub.add_parser("serve", help="offer the SSH server and your key to the monitor")
    s.add_argument("--monitor", default="192.168.111.1", help="the monitor's address in recovery mode (default %(default)s)")
    s.add_argument("--port", type=int, default=8000)
    s.add_argument("--dist", default=str(TOP / "dist"), help="folder with dropbearmulti (default: dist)")
    s.add_argument("--key", default="~/.ssh/id_ed25519.pub", help="your public SSH key (default %(default)s)")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("backup", help="copy the monitor's flash to this PC and verify it")
    s.add_argument("monitor", help="the monitor's address (192.168.111.1 in recovery mode)")
    s.add_argument("--out", default="backup", help="folder for the backup (default: backup)")
    s.set_defaults(fn=cmd_backup)

    s = sub.add_parser("install", help="install the on-device components")
    s.add_argument("monitor", help="the monitor's address on your home network")
    s.add_argument("--conf", default="bridge.conf", help="your MQTT settings (default: bridge.conf)")
    s.add_argument("--cert", default="broker.pem", help="broker certificate; fetched automatically if missing")
    s.add_argument("--ha-ip", help="Home Assistant address, if different from the MQTT broker's")
    s.add_argument("--dist", default=str(TOP / "dist"), help="folder with vimarbridge, vimaraudio and vimarlog (default: dist)")
    s.add_argument("--work", default="work", help="folder for files copied from the monitor (default: work)")
    s.add_argument("--no-lua", action="store_true", help="do not install the Lua patch")
    s.add_argument("--no-keylock", action="store_true", help="do not install the key lock")
    s.add_argument("--reboot", action="store_true", help="restart the monitor when done")
    s.set_defaults(fn=cmd_install)

    s = sub.add_parser("getkey", help="read the video stream key from the monitor (no cloud)")
    s.add_argument("monitor", help="the monitor's address on your home network")
    s.add_argument("--auth", help="auth code (the code on the monitor's back label)")
    s.add_argument("--from", dest="from_file", help="take auth_code from an existing stream_credentials.json")
    s.add_argument("--out", default="stream_credentials.json", help="file to write (default: %(default)s)")
    s.add_argument("--verify", help="compare the key with this existing stream_credentials.json and stop")
    s.add_argument("--no-write", action="store_true", help="only read/verify, do not write the file")
    s.add_argument("--label", default="DeviceKey2", help="field-name label next to the key (default: %(default)s)")
    s.add_argument("--key", help="use this stream key instead of reading it from the monitor")
    s.add_argument("--auth-open", default="app_token[", help="marker before the auth token in memory")
    s.add_argument("--auth-close", default="]", help="marker after the auth token in memory")
    s.add_argument("--dist", default=str(TOP / "dist"), help="folder with vgetkey (default: dist)")
    s.set_defaults(fn=cmd_getkey)

    s = sub.add_parser("status", help="show what is running on the monitor")
    s.add_argument("monitor")
    s.set_defaults(fn=cmd_status)

    a = p.parse_args()
    if not a.cmd:
        p.print_help()
        sys.exit(2)
    a.fn(a)


if __name__ == "__main__":
    main()
