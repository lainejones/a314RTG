#!/usr/bin/env python3
# Dump a Picasso96Settings IFF (FORM P96S): list chunks, STHD board types,
# board/monitor names, and resolution entries - to spot phantom boards and
# how modes bind. Read-only.
import sys, struct

d = open(sys.argv[1], "rb").read()

def asc(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)

if d[:4] != b"FORM":
    print("not an IFF FORM:", d[:4]); sys.exit(1)
print("FORM size=%d type=%s  filelen=%d" %
      (struct.unpack(">I", d[4:8])[0], d[8:12].decode("latin1"), len(d)))

o = 12
counts = {}
boards = []
while o + 8 <= len(d):
    cid = d[o:o+4].decode("latin1")
    sz  = struct.unpack(">I", d[o+4:o+8])[0]
    o  += 8
    body = d[o:o+sz]
    counts[cid] = counts.get(cid, 0) + 1
    extra = ""
    if cid == "STHD" and sz >= 8:
        bt = struct.unpack(">I", body[:4])[0]
        nm = asc(body[8:40]).split(".")[0]
        extra = "  BoardType=%d  name=%r" % (bt, nm)
        boards.append((bt, nm))
    elif cid in ("BDNM", "MNTR", "NAME"):
        extra = "  " + repr(asc(body).split(".")[0])
    print("%-5s sz=%-5d%s | %s" % (cid, sz, extra, asc(body[:56])))
    o += sz + (sz & 1)

print("\nchunk counts:", counts)
print("STHD boards (%d):" % len(boards), boards)
