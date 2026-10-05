#!/usr/bin/env python3
"""luxp_dump.py <file.luxp>: writes the plain bytes of every section of every
page (zlib sections inflated), so a test can look for a string in them and
for HTML source that must not be there.  Exit 1 if the file is not a .luxp."""
import sys, zlib

data = open(sys.argv[1], "rb").read()
if data[:4] != b"LUXP" or data[4] != 5:
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
    for _ in range(3):                   # document, sheet, program
        z = data[at]; at += 1
        uv()                             # plain size
        blob = s()
        out += zlib.decompress(blob) if z else blob
for _ in range(uv()):
    s(); out += s()
sys.stdout.buffer.write(out)
