#!/bin/sh
# shellcheck disable=SC2086  # $F and the URL lists are split into words on purpose
# Build the ARM programs that run on the monitor, into dist/:
#
#   dist/dropbearmulti   SSH server (Dropbear, patched for this monitor)
#   dist/vimarbridge     door-bus <-> MQTT bridge
#   dist/vimaraudio      door audio helper
#   dist/vimarlog        non-blocking local diagnostic log sender
#   dist/vgetkey         temporary stream-key reader
#
# You only need this if you do not use the ready-made files from the Releases page.
# Runs on Debian / Ubuntu, and on Windows inside WSL (see docs/building.md):
#
#   sudo apt install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross make curl bzip2
#   ./build.sh
#
# The sources of Dropbear and BearSSL are downloaded from their official sites and checked
# against the SHA-256 values below. To build without internet, put the two archives into
# build/src/ first (or set BEARSSL_SRC=<unpacked bearssl-0.6 folder>).
set -eu

cd "$(dirname "$0")"
TOP=$PWD
DIST=$TOP/dist
WORK=$TOP/build
SRC=$WORK/src

CC=${CC:-arm-linux-gnueabi-gcc}
CROSS=${CC%gcc}
# Mandatory for the monitor's CPU (Goke GK7101S = ARM1176, ARMv6, soft-float). Do not change:
# without -fno-strict-aliasing the SSH handshake and the TLS code break; armv7 code does not run.
F="-O1 -march=armv6 -marm -mfloat-abi=soft -fno-strict-aliasing -fno-strict-overflow -mno-unaligned-access"

DROPBEAR=dropbear-2025.89
DROPBEAR_SHA256=0d1f7ca711cfc336dc8a85e672cab9cfd8223a02fe2da0a4a7aeb58c9e113634
DROPBEAR_URLS="https://matt.ucc.asn.au/dropbear/releases/$DROPBEAR.tar.bz2 https://dropbear.nl/mirror/releases/$DROPBEAR.tar.bz2"
BEARSSL=bearssl-0.6
BEARSSL_SHA256=6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14
BEARSSL_URLS="https://bearssl.org/$BEARSSL.tar.gz"

say() { printf '\n== %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

command -v "$CC" >/dev/null 2>&1 || die "$CC not found. Install it:  sudo apt install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross make curl bzip2"
command -v make >/dev/null 2>&1 || die "make not found. Install it:  sudo apt install -y make"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found"

# fetch <file name> <sha256> <url>...   -> $SRC/<file name>, checksum verified
fetch() {
    name=$1; sum=$2; shift 2
    if [ ! -f "$SRC/$name" ]; then
        for url in "$@"; do
            echo "downloading $url"
            if command -v curl >/dev/null 2>&1; then
                curl -fL --retry 2 -o "$SRC/$name.part" "$url" && mv "$SRC/$name.part" "$SRC/$name" && break
            else
                wget -O "$SRC/$name.part" "$url" && mv "$SRC/$name.part" "$SRC/$name" && break
            fi
        done
        rm -f "$SRC/$name.part"
        [ -f "$SRC/$name" ] || die "could not download $name (put the file into $SRC/ yourself and run again)"
    fi
    got=$(sha256sum "$SRC/$name" | cut -c1-64)
    [ "$got" = "$sum" ] || die "$name has the wrong checksum ($got) - delete $SRC/$name and run again"
}

mkdir -p "$DIST" "$SRC"

# ---------------------------------------------------------------- 1. dropbear (SSH server)
say "1/5 dropbear ($DROPBEAR)"
fetch "$DROPBEAR.tar.bz2" "$DROPBEAR_SHA256" $DROPBEAR_URLS
rm -rf "${WORK:?}/$DROPBEAR"
tar -xjf "$SRC/$DROPBEAR.tar.bz2" -C "$WORK"
cd "$WORK/$DROPBEAR"

# key-only login (no passwords)
cat > localoptions.h <<'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM 0
EOF
# no server-side extension info
sed -i 's/^#define DROPBEAR_EXT_INFO.*/#define DROPBEAR_EXT_INFO 0/' src/sysoptions.h
sed -i '/^#define DROPBEAR_EXT_INFO 0/{n;/DROPBEAR_CLI_PUBKEY_AUTH/d}' src/sysoptions.h
# The monitor's firmware ships "/" world-writable, so dropbear's permission check on the path of
# authorized_keys can never pass there. Skip that check (single-user appliance, key-only login).
sed -i 's/\tTRACE(("enter checkpubkeyperms"))/\tTRACE(("enter checkpubkeyperms"))\n\tm_free(path); return DROPBEAR_SUCCESS; \/* 0777-rootfs device *\//' src/svr-authpubkey.c
grep -q '^#define DROPBEAR_EXT_INFO 0$' src/sysoptions.h || die "dropbear patch 1 did not apply (source changed?)"
grep -q 'return DROPBEAR_SUCCESS; /\* 0777-rootfs device \*/' src/svr-authpubkey.c || die "dropbear patch 2 did not apply (source changed?)"

./configure --host=arm-linux-gnueabi --disable-zlib --disable-harden \
    CC="$CC" CFLAGS="$F" LDFLAGS="-static" > "$WORK/dropbear-configure.log" 2>&1 \
    || die "dropbear configure failed, see $WORK/dropbear-configure.log"
make -j"$(nproc 2>/dev/null || echo 2)" PROGRAMS="dropbear dropbearkey" MULTI=1 STATIC=1 > "$WORK/dropbear-make.log" 2>&1 \
    || die "dropbear build failed, see $WORK/dropbear-make.log"
"${CROSS}strip" dropbearmulti
cp dropbearmulti "$DIST/dropbearmulti"

# ---------------------------------------------------------------- 2. vimarbridge (needs BearSSL)
say "2/5 vimarbridge (with $BEARSSL)"
if [ -n "${BEARSSL_SRC:-}" ]; then
    B=$BEARSSL_SRC
    [ -f "$B/inc/bearssl.h" ] || die "BEARSSL_SRC=$B is not an unpacked BearSSL source folder"
else
    fetch "$BEARSSL.tar.gz" "$BEARSSL_SHA256" $BEARSSL_URLS
    rm -rf "${WORK:?}/$BEARSSL"
    tar -xzf "$SRC/$BEARSSL.tar.gz" -C "$WORK"
    B=$WORK/$BEARSSL
fi
OBJ=$WORK/bearssl-obj
rm -rf "$OBJ"; mkdir -p "$OBJ"
cd "$B"
find src -name '*.c' | while read -r f; do
    "$CC" $F -Iinc -Isrc -c "$f" -o "$OBJ/$(echo "$f" | tr / _).o" || exit 1
done || die "BearSSL build failed"
rm -f "$WORK/libbearssl.a"
"${CROSS}ar" rcs "$WORK/libbearssl.a" "$OBJ"/*.o
cd "$TOP"
"$CC" $F -static -I"$B/inc" -o "$DIST/vimarbridge" on-device/vimarbridge.c "$WORK/libbearssl.a" 2> "$WORK/vimarbridge.log" \
    || { cat "$WORK/vimarbridge.log" >&2; die "vimarbridge build failed"; }

# ---------------------------------------------------------------- 3. vimaraudio
say "3/5 vimaraudio"
"$CC" $F -static -Wall -o "$DIST/vimaraudio" on-device/vimaraudio/vimaraudio.c 2> "$WORK/vimaraudio.log" \
    || { cat "$WORK/vimaraudio.log" >&2; die "vimaraudio build failed"; }

# ---------------------------------------------------------------- 4. vimarlog (non-blocking diagnostic sender)
say "4/5 vimarlog"
"$CC" $F -static -Wall -o "$DIST/vimarlog" on-device/vimarlog.c 2> "$WORK/vimarlog.log" \
    || { cat "$WORK/vimarlog.log" >&2; die "vimarlog build failed"; }

# ---------------------------------------------------------------- 5. vgetkey (stream-key reader)
say "5/5 vgetkey"
"$CC" $F -static -Wall -o "$DIST/vgetkey" on-device/getkey/vgetkey.c 2> "$WORK/vgetkey.log" \
    || { cat "$WORK/vgetkey.log" >&2; die "vgetkey build failed"; }

# ---------------------------------------------------------------- check: ARM, static
say "checking the results"
for f in dropbearmulti vimarbridge vimaraudio vimarlog vgetkey; do
    h=$("${CROSS}readelf" -h "$DIST/$f")
    echo "$h" | grep -q 'Machine: *ARM' || die "$f is not an ARM program"
    "${CROSS}readelf" -l "$DIST/$f" | grep -q 'INTERP' && die "$f is not statically linked"
    echo "ok  $f  ($(wc -c < "$DIST/$f") bytes)"
done
( cd "$DIST" && sha256sum dropbearmulti vimarbridge vimaraudio vimarlog vgetkey > SHA256SUMS )

say "done - the files are in $DIST"
