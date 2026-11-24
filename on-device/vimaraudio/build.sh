#!/bin/sh
set -eu

# K40955 / Goke GK7101S (ARMv6) build flags. These are mandatory (see docs/03).
F="-O1 -march=armv6 -marm -mfloat-abi=soft -fno-strict-aliasing -fno-strict-overflow -mno-unaligned-access -static"
CC=${CC:-arm-linux-gnueabi-gcc}

mkdir -p build
$CC $F -Wall -Wextra -o build/vimaraudio src/vimaraudio.c

if [ "${1:-}" = "bridge" ]; then
    : "${BEARSSL:?Set BEARSSL to the BearSSL 0.6 build directory}"
    $CC $F -I"$BEARSSL/inc" -o build/vimarbridge ../vimarbridge.c "$BEARSSL/build/libbearssl.a"
fi
