#!/usr/bin/env python3
# Build a clean Picasso96Settings: the board header (ANNO/STHD/MNTR/BDNM) plus
# ONLY the "no board:" resolutions from a source file. Drops every board-prefixed
# (a314rtg:/A314RTG:) RSHD, which otherwise registers a PHANTOM board that competes
# with the live card and stops modes from routing.
#   build_clean_p96.py <source-settings> <output>
import sys, struct

src = open(sys.argv[1], "rb").read()

def parse(d):
    o, out = 12, []
    while o + 8 <= len(d):
        cid = d[o:o+4]; sz = struct.unpack(">I", d[o+4:o+8])[0]; o += 8
        out.append((cid, d[o:o+sz])); o += sz + (sz & 1)
    return out

def asc(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)

cl = parse(src)
hdr = [c for c in cl if c[0] in (b"ANNO", b"STHD", b"MNTR", b"BDNM")]
out = list(hdr)

i = 0; kept = []; skipped = 0
while i < len(cl):
    cid, body = cl[i]
    if cid == b"RSHD":
        nm = asc(body)
        grp = [(cid, body)]; j = i + 1
        while j < len(cl) and cl[j][0] == b"MIHD":
            grp.append(cl[j]); j += 1
        if "no board:" in nm:
            out += grp
            k = nm.find("no board:")
            kept.append(nm[k:k+20])
        else:
            skipped += 1
        i = j
    else:
        i += 1

def chunk(cid, body):
    o = cid + struct.pack(">I", len(body)) + body
    return o + (b"\x00" if len(body) & 1 else b"")

payload = b"P96S" + b"".join(chunk(c, b) for c, b in out)
form = b"FORM" + struct.pack(">I", len(payload)) + payload
open(sys.argv[2], "wb").write(form)

print("wrote", sys.argv[2], len(form), "bytes")
print("header:", [c.decode() for c, _ in hdr])
print("kept", len(kept), "no-board modes; dropped", skipped, "phantom RSHDs")
print("has 800x600:", any("800x600" in k for k in kept))
