#!/usr/bin/env python3
# Dump each RSHD matching a name filter with all its MIHD modeinfos in full hex,
# so we can diff the mode that works vs the one that greys.
#   mihd_diff.py <settings> <namefilter>
import sys, struct
d = open(sys.argv[1], "rb").read()
flt = sys.argv[2]

def asc(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)

o = 12; show = False
while o + 8 <= len(d):
    cid = d[o:o+4]; sz = struct.unpack(">I", d[o+4:o+8])[0]; o += 8
    body = d[o:o+sz]
    if cid == b"RSHD":
        nm = asc(body); show = flt in nm
        if show:
            k = nm.find(flt)
            print("RSHD %r" % nm[max(0,k-9):k+9])
    elif cid == b"MIHD" and show:
        # words for quick scan
        w = [struct.unpack_from(">H", body, i)[0] for i in range(0, len(body)-1, 2)]
        print("  MIHD[%d]: %s" % (sz, body.hex()))
        print("         words=%s" % w)
    o += sz + (sz & 1)
