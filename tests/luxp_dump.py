#!/usr/bin/env python3
"""luxp_dump.py <file.luxp>: writes the plain bytes of every section of every
page (zlib sections inflated), so a test can look for a string in them and
for HTML source that must not be there.  Exit 1 if the file is not a .luxp."""
import sys, zlib

data = open(sys.argv[1], "rb").read()
if data[:4] != b"LUXP" or data[4] not in (5, 6, 7):
    sys.exit(1)
at = 5


def uv():
    global at
    v, shift = 0, 0
    while True:
        b = data[at]; at += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v
        shift += 7


def s():
    global at
    n = uv(); v = data[at:at + n]; at += n
    return v


out = b""
for _ in range(uv()):
    s()                                  # name
    traced = data[at] & 3 == 3           # the document as template block + trace of values
    if traced:
        at += 1
    for _ in range(4 if traced else 3):  # [template, trace | document], sheet, program
        f = data[at]; at += 1
        if f & 3 == 2:                   # a reference to a block the reader holds: nothing to dump
            at += 8
            continue
        if f & 4:                        # the block's id
            at += 8
        uv()                             # plain size
        blob = s()
        out += zlib.decompress(blob) if f & 3 == 1 else blob
for _ in range(uv()):
    s(); out += s()
sys.stdout.buffer.write(out)
