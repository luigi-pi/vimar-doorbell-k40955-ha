#!/usr/bin/env python3
"""B1 patch (T52) for the monitor's luaprj/src/base/base.lua (Qualvision, Lua 5.3, GBK + CRLF).
Two changes, bytes elsewhere unchanged:
  1. ring handler (__Base:MsgOutdoorCallProc): a LAN viewer is kept (stock: outcall_cloud_video_close_proc
     freezes it, CloudDisconnect closes it 10 s later, T46/T51)
  2. viewer start (__Base:MsgPhoneStartMonitorProc): with the call page open, record the viewer and return 1
     without closing the page (stock closes it: 32 + 17, ringtone left on, T46/T47). Must return non-zero, else
     mainapp re-sends MSG_WIFI_START_MONITOR every 100 ms (T46).
usage: python3 -I make_patch.py <original base.lua> <patched output>"""
import hashlib, sys

ORIG, PATCHED = "77d3402119e91d34042874b2a776fc93", "88d80b8b01aea3c9d2f1eb3f55c1c627"
OLD1 = (b'\tif 1 == LUA_CALLC("get_phone_monitor_flag") then\r\n'
        b'\t    LUA_CALLC("outcall_cloud_video_close_proc",1)\r\n'
        b'        print("=============== MsgOutdoorCallProc PhoneChannel = ",PhoneChannel)\r\n'
        b'        LUA_CALLC("CloudDisconnect")\r\n'
        b'        LUA_CALLC("set_phone_monitor_flag", 0)\r\n'
        b'\tend\r\n')
NEW1 = (b'\tif 1 == LUA_CALLC("get_phone_monitor_flag") then\r\n'
        b'\t    -- K40955 local patch T52: keep the LAN viewer during the ring (stock: freeze, CloudDisconnect)\r\n'
        b'        print("=============== MsgOutdoorCallProc viewer kept, PhoneChannel = ",PhoneChannel)\r\n'
        b'\tend\r\n')
OLD2 = b'\tlocal ActiveWnd = LUA_CALLC("GetActiveWindow")\r\n\tflag_phone = 1\r\n'
NEW2 = (b'\tlocal ActiveWnd = LUA_CALLC("GetActiveWindow")\r\n'
        b'\t-- K40955 local patch T52: call page open (ring or call): keep it, only record the viewer\r\n'
        b'\tif ResTbl.PageId == PAGEID.PAGE_ID_CAM then\r\n'
        b'\t\tLUA_CALLC("set_phone_monitor_flag", 1)\r\n'
        b'\t\tPhoneChannel = Chn\r\n'
        b'\t\treturn 1\r\n'
        b'\tend\r\n'
        b'\tflag_phone = 1\r\n')

src = open(sys.argv[1], "rb").read()
if hashlib.md5(src).hexdigest() != ORIG:
    sys.exit("input is not the analysed base.lua (md5 %s)" % ORIG)
out = src.replace(OLD1, NEW1, 1).replace(OLD2, NEW2, 1)
if hashlib.md5(out).hexdigest() != PATCHED:
    sys.exit("patch result unexpected")
open(sys.argv[2], "wb").write(out)
print("written", sys.argv[2], PATCHED)
