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

HQ video (--hq): the same copy, plus the live view on the monitor's existing 896x576 stream (the one its own
recordings use) instead of 352x288. Three more instructions; the encoder settings are not touched. Installed
as /mntfs/vimarbridge/app-hq/mainapp and switched on from Home Assistant (switch "High quality video").
Do not set the monitor's REC button to video with HQ on: recording and live view would share that stream.

usage: python3 make_keylock.py [--hq] <mainapp copied from the monitor> <output file>
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
HQ_PATCHES = [
    (0x000E47C4, 0xE3A01001, 0xE3A01000),  # mov r1,#1 -> #0: live view on encoder stream 1 instead of stream 2
    (0x0012F3B8, 0xE3A03E16, 0xE3A03E38),  # mov r3,#0x160 -> #0x380: width 352 -> 896
    (0x0012F3C0, 0xE3A03E12, 0xE3A03E24),  # mov r3,#0x120 -> #0x240: height 288 -> 576
]


def keylock(data, hq=False):
    """Return the patched copy of the stock mainapp (with hq: also the 896x576 live view), or raise ValueError."""
    if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
        raise ValueError("input is not the analysed mainapp (sha256 %s...)" % STOCK_SHA256[:12])
    out = bytearray(data)
    for va, old, new in PATCHES + (HQ_PATCHES if hq else []):
        off = va - TEXT_VA + TEXT_OFF
        cur = struct.unpack_from("<I", out, off)[0]
        if cur != old:
            raise ValueError("unexpected instruction at %#x: %#010x (expected %#010x)" % (va, cur, old))
        struct.pack_into("<I", out, off, new)
    return bytes(out)


if __name__ == "__main__":
    args = sys.argv[1:]
    hq = args[:1] == ["--hq"]
    if hq:
        args = args[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    try:
        patched = keylock(open(args[0], "rb").read(), hq)
    except ValueError as e:
        sys.exit("%s - nothing written" % e)
    open(args[1], "wb").write(patched)
    print("written", args[1], hashlib.sha256(patched).hexdigest()[:8], "(key lock + HQ video)" if hq else "(key lock)")
