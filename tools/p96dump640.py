#!/usr/bin/env python3
# Dump the RSHD (+ its MIHD modeinfos) for every 640x480 resolution in a
# Picasso96Settings IFF, as hex, so working vs broken entries can be diffed.
import sys, struct
d = open(sys.argv[1], "rb").read()
def asc(b): return "".join(chr(c) if 32 <= c < 127 else "." for c in b)
o = 12
show = False
while o + 8 <= len(d):
    cid = d[o:o+4].decode("latin1"); sz = struct.unpack(">I", d[o+4:o+8])[0]; o += 8
    body = d[o:o+sz]
    if cid == "RSHD":
        nm = asc(body)
        show = "640x480" in nm
        if show:
            print("RSHD name=%r  sz=%d" % (nm, sz))
            print("   ", body.hex())
    elif cid == "MIHD" and show:
        print("  MIHD sz=%d" % sz)
        print("   ", body.hex())
    elif cid in ("RSHD",):
        pass
    o += sz + (sz & 1)
