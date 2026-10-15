#!/usr/bin/env python3
"""Tile PNG snapshots into one image: contact_sheet.py out.png cols in1.png in2.png ..."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
from compare import read_png  # noqa: E402


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    imgs = [read_png(p) for p in files]
    w, h = imgs[0][0], imgs[0][1]
    nrows = (len(imgs) + cols - 1) // cols
    sheet = [[(40, 40, 40)] * (cols * (w + 2)) for _ in range(nrows * (h + 2))]
    for i, (_, _, rows) in enumerate(imgs):
        ox, oy = (i % cols) * (w + 2), (i // cols) * (h + 2)
        for y in range(h):
            sheet[oy + y][ox:ox + w] = rows[y]
    write_png(out, cols * (w + 2), nrows * (h + 2), sheet)


if __name__ == "__main__":
    main()
