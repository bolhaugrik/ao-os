#!/usr/bin/env python3
"""AOFS v1 image-epito: egy konyvtar tartalmabol csak-olvashato ramdisk-kepet keszit.

  superblock (4096 B): magic "AOFS", u32 version=1, u32 count, u32 total_size
  entry-tabla:         count x 128 B: name[96], u32 type, u32 offset, u32 size, u32 flags, pad[12]
  adatterulet:         fajlok 16 bajtra igazitva
"""
import argparse
import os
import struct

ENTRY = 128
SUPER = 4096
FILE, DIR = 1, 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    entries = []  # (name, type, data)
    for dirpath, dirnames, filenames in os.walk(a.root):
        dirnames.sort()
        rel = os.path.relpath(dirpath, a.root).replace(os.sep, "/")
        if rel != ".":
            entries.append((rel, DIR, b""))
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            name = fn if rel == "." else f"{rel}/{fn}"
            if len(name.encode()) > 95:
                raise SystemExit(f"tul hosszu nev: {name}")
            entries.append((name, FILE, open(p, "rb").read()))

    table = bytearray()
    data = bytearray()
    base = SUPER + len(entries) * ENTRY
    base = (base + 15) & ~15
    for name, typ, blob in entries:
        off = base + len(data) if typ == FILE else 0
        table += struct.pack("<96sIIII12x", name.encode("latin-1"), typ, off, len(blob), 0)
        if typ == FILE:
            data += blob
            while len(data) % 16:
                data += b"\0"

    total = base + len(data)
    img = bytearray(total)
    struct.pack_into("<4sIII", img, 0, b"AOFS", 1, len(entries), total)
    img[SUPER:SUPER + len(table)] = table
    img[base:base + len(data)] = data
    with open(a.output, "wb") as f:
        f.write(img)
    print(f"aofs: {a.output}  {len(entries)} bejegyzes, {total} B")


if __name__ == "__main__":
    main()
