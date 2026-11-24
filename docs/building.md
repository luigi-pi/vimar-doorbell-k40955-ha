# Building the monitor programs yourself

**You do not need this page if you use the kit zip from the
[Releases page](https://github.com/luigi-pi/vimar-doorbell-k40955-ha/releases)** — its `dist` folder
already contains the monitor programs and the temporary stream-key reader, built automatically from this repository by
[GitHub Actions](../.github/workflows/build.yml).

Build them yourself if there is no release yet, if you changed the source, or if you prefer not to
run programs someone else compiled.

| Program | What it is |
|---|---|
| `dist/dropbearmulti` | the SSH server ([Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html) 2025.89 with two small changes) |
| `dist/vimarbridge` | the door‑bus ⇄ MQTT bridge (`on-device/vimarbridge.c` + [BearSSL](https://bearssl.org/) 0.6) |
| `dist/vimaraudio` | the door audio helper (`on-device/vimaraudio/vimaraudio.c`) |
| `dist/vimarlog` | non-blocking local diagnostic sender (`on-device/vimarlog.c`) |
| `dist/vgetkey` | temporary reader used by `tools/vimar.py getkey` |

## Linux (Debian 13, Ubuntu 24.04 or newer)

In the kit folder:

```sh
sudo apt update
sudo apt install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross make curl bzip2
sh build.sh
```

`build.sh` downloads the Dropbear and BearSSL sources from their official sites, checks them against
fixed SHA‑256 checksums, builds everything and verifies that the results are static ARM programs.
It ends with `done - the files are in …/dist`.

> Use a recent system (the ready‑made files are built on Ubuntu 24.04). With the C library of older
> systems the SSH server can build fine and then refuse every login on the monitor.

## Windows

Windows cannot build these programs directly, but its built‑in Linux environment (WSL) can.

1. Open *PowerShell* **as administrator**, run `wsl --install`, restart the PC and finish the Ubuntu
   set‑up (it asks for a new user name and password).
2. Open **Ubuntu** from the Start menu and copy the kit into it (replace the path with yours;
   `C:\Users\you\Downloads` is `/mnt/c/Users/you/Downloads` there):

   ```sh
   cp -r /mnt/c/Users/you/Downloads/vimar-k40955-kit ~/kit
   cd ~/kit
   sudo apt update
   sudo apt install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross make curl bzip2
   sh build.sh
   ```

3. Copy the result back into the kit folder on Windows:

   ```sh
   cp -r dist /mnt/c/Users/you/Downloads/vimar-k40955-kit/
   ```

Everything else in the guide is done in normal Windows, not in Ubuntu.

## Via GitHub (no local tools)

Fork the repository on GitHub, open the fork's **Actions** tab, run the workflow *Build the kit* and
download the artifact `vimar-k40955-kit` from the finished run. It is the same zip as on the Releases
page, built from your fork.

## What `build.sh` does, and why

The monitor's CPU is a Goke GK7101S: **ARM1176, ARMv6, soft‑float**. (Its `/proc/cpuinfo` says
*"CPU architecture: 7"*, but ARMv7 code does not run on it.) All five binaries are built **static**
with these flags:

```
-O1 -march=armv6 -marm -mfloat-abi=soft -fno-strict-aliasing -fno-strict-overflow -mno-unaligned-access
```

| | |
|---|---|
| `-march=armv6 -marm -mfloat-abi=soft` | the CPU; anything newer crashes with *Illegal instruction* |
| `-fno-strict-aliasing` | without it the compiler breaks Dropbear's handshake (*"no matching cipher"*) and the TLS code |
| `-static` | the monitor has no matching system libraries |

Dropbear is configured for key‑only logins (`DROPBEAR_SVR_PASSWORD_AUTH 0`) and gets two changes:

1. `DROPBEAR_EXT_INFO 0` — no server‑side extension info.
2. The permission check on the path of `authorized_keys` is skipped. The monitor's firmware ships its
   root folder `/` world‑writable and owned by another user, so that check can never pass there; on
   this single‑user appliance with key‑only login, skipping it is appropriate.

Both are plain `sed` lines in `build.sh`; the script stops if they do not apply.

To build without internet access, put `dropbear-2025.89.tar.bz2` and `bearssl-0.6.tar.gz` into
`build/src/` first.

## Licences of the built programs

See [THIRD-PARTY.md](../THIRD-PARTY.md).
