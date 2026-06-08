#!/usr/bin/env python3
# Build a minimal Picasso96Settings (FORM P96S) containing ONLY the working
# "no board:640x480" resolution (verbatim bytes from a source settings file),
# plus the ANNO/STHD/BDNM header chunks. No useless default modes.
#   build_p96.py <source-settings> <output>
import sys, struct

src = open(sys.argv[1], "rb").read()

def parse(d):
    o, out = 12, []
    while o + 8 <= len(d):
        cid = d[o:o+4]; sz = struct.unpack(">I", d[o+4:o+8])[0]; o += 8
        out.append((cid, d[o:o+sz])); o += sz + (sz & 1)
    return out

cl = parse(src)
anno = next((c for c in cl if c[0] == b"ANNO"), None)
sthd = next((c for c in cl if c[0] == b"STHD"), None)
bdnm = next((c for c in cl if c[0] == b"BDNM"), None)

res = []
for i, (cid, body) in enumerate(cl):
    if cid == b"RSHD" and b"no board:640x480" in body:
        res.append((cid, body))
        j = i + 1
        while j < len(cl) and cl[j][0] == b"MIHD":
            res.append(cl[j]); j += 1
        break
if not res:
    sys.exit("no 'no board:640x480' RSHD found in source")

order = [c for c in (anno, sthd, bdnm) if c] + res

def chunk(cid, body):
    out = cid + struct.pack(">I", len(body)) + body
    return out + (b"\x00" if len(body) & 1 else b"")

payload = b"P96S" + b"".join(chunk(c, b) for c, b in order)
form = b"FORM" + struct.pack(">I", len(payload)) + payload
open(sys.argv[2], "wb").write(form)
print("wrote", sys.argv[2], "size", len(form),
      "chunks:", [c.decode("latin1") for c, _ in order])
