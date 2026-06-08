#!/usr/bin/env python3
# For each RSHD whose name matches a filter, decode its MIHD modeinfos and report
# the width/height words found, so we can spot a bad height (e.g. 800x600 modeinfo
# that actually carries 300). Usage: mihd_dims.py <settings> <namefilter>
import sys, struct
d = open(sys.argv[1], "rb").read()
flt = sys.argv[2] if len(sys.argv) > 2 else ""

def asc(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)

def words(b):
    return [struct.unpack_from(">H", b, i)[0] for i in range(0, len(b)-1, 2)]

o = 12; cur = None; show = False
while o + 8 <= len(d):
    cid = d[o:o+4]; sz = struct.unpack(">I", d[o+4:o+8])[0]; o += 8
    body = d[o:o+sz]
    if cid == b"RSHD":
        nm = asc(body); k = nm.find("no board:")
        cur = nm[k:k+18] if k >= 0 else nm[:18]
        show = flt in cur
        if show: print("RSHD", repr(cur))
    elif cid == b"MIHD" and show:
        w = words(body)
        # heuristic: find 800(0x320) and the value right after as height candidate
        cand = [(i, w[i], w[i+1]) for i in range(len(w)-1) if w[i] in (320, 640, 800, 1024)]
        print("  MIHD sz=%d  w/h pairs near width-like vals: %s" %
              (sz, [(a, b) for _, a, b in cand][:4]))
    o += sz + (sz & 1)
