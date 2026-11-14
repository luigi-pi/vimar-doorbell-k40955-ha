#!/usr/bin/env python3
"""Key lock for the monitor's main program (mainapp, firmware V401R001B002).

Why: the monitor changes its video "stream key" by itself every 7 days, even with the internet
blocked. The key saved in Home Assistant (stream_credentials.json) then stops working and the video
goes dark until a new key is fetched from the cloud. This tool switches that weekly change off.

How: it makes a copy of YOUR OWN mainapp in which one instruction is replaced by "do nothing"
(the call to refresh_token() in CTDKCloudToken::PumpEvts, address 0x0069b974). The original on the
monitor is never modified: the copy goes to the data partition (/mntfs/vimarbridge/app/mainapp) and
boot.sh starts it instead; if it does not start, boot.sh falls back to the original by itself.

The input is checked byte for byte (SHA-256 of the analysed build, then the instruction itself):
anything else is refused and nothing is written. No vendor code is contained in this repository.

usage: python3 make_keylock.py <mainapp copied from the monitor> <output file>
(tools/vimar.py install does this for you.)
"""
import hashlib
import struct
import sys

STOCK_SHA256 = "3075dcf9e5ab51a2244b3a8e443efc39d97eb7e9c030638f2b7a07e54e2fd5aa"
TEXT_VA, TEXT_OFF = 0x8000, 0x0          # first LOAD segment: file offset = address - 0x8000
NOP = 0xE1A00000                         # mov r0, r0
PATCHES = [                              # address, original word, new word
    (0x0069B974, 0xEBFFF858, NOP),       # bl CTDKCloudToken::refresh_token  ->  nop
]


def keylock(data):
    """Return the patched copy of the stock mainapp, or raise ValueError."""
    if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
        raise ValueError("input is not the analysed mainapp (sha256 %s...)" % STOCK_SHA256[:12])
    out = bytearray(data)
    for va, old, new in PATCHES:
        off = va - TEXT_VA + TEXT_OFF
        cur = struct.unpack_from("<I", out, off)[0]
        if cur != old:
            raise ValueError("unexpected instruction at %#x: %#010x (expected %#010x)" % (va, cur, old))
        struct.pack_into("<I", out, off, new)
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        patched = keylock(open(sys.argv[1], "rb").read())
    except ValueError as e:
        sys.exit("%s - nothing written" % e)
    open(sys.argv[2], "wb").write(patched)
    print("written", sys.argv[2], hashlib.sha256(patched).hexdigest()[:8])
