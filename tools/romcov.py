#!/usr/bin/env python3
"""Summarise ROM usage maps written by `batman-rt --romcov`.

    romcov.py map.bin [more.bin ...] [--png out.png] [--runs] [--gap N]

Several maps (from different play sessions) are merged. Prints how many words each user
touched per 64K-word segment and whether the upper ROM half was read. --runs lists the
contiguous regions by kind, which is what notes/rom-usage.md is built from. --png draws the
whole 4M-word space, 16 words per pixel, 512 pixels (8192 words) per row:
red = code fetch, yellow = CPU data, green = PPU (graphics), blue = SPU (sound)."""
import array
import os
import struct
import sys
import zlib

SPACE = 0x400000
FETCH, DATA, PPU, SPU = 1, 2, 4, 8
NAMES = {FETCH: "code", DATA: "data", PPU: "gfx", SPU: "sound"}


def load(paths):
    flags = bytearray(SPACE)
    first = array.array("I", [0xFFFFFFFF]) * SPACE
    for p in paths:
        with open(p, "rb") as f:
            fl = f.read(SPACE)
            fi = array.array("I")
            fi.frombytes(f.read(SPACE * 4))
        for i, v in enumerate(fl):
            if v:
                flags[i] |= v
                if fi[i] < first[i]:
                    first[i] = fi[i]
    return flags, first


def kind(v):
    """the most specific class: code beats data beats gfx beats sound"""
    for k in (FETCH, DATA, PPU, SPU):
        if v & k:
            return k
    return 0


def runs(flags, first, gap):
    out = []
    i = 0
    while i < SPACE:
        k = kind(flags[i])
        if not k:
            i += 1
            continue
        start, end, f0 = i, i, first[i]
        j = i + 1
        while j < SPACE and j - end <= gap:
            if flags[j]:
                if kind(flags[j]) != k:
                    break
                end = j
                f0 = min(f0, first[j])
            j += 1
        out.append((start, end, k, f0))
        i = end + 1
    return out


def png(path, flags):
    colours = {0: (24, 24, 28), FETCH: (230, 60, 50), DATA: (240, 200, 40), PPU: (60, 200, 90), SPU: (70, 120, 240)}
    w, per = 512, 16
    h = SPACE // per // w
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            base = (y * w + x) * per
            acc = 0
            for v in flags[base:base + per]:
                acc |= v
            raw += bytes(colours[kind(acc)])
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    out_png, show_runs, gap = None, False, 16
    paths = []
    while args:
        a = args.pop(0)
        if a == "--png":
            out_png = args.pop(0)
        elif a == "--runs":
            show_runs = True
        elif a == "--gap":
            gap = int(args.pop(0))
        else:
            paths.append(a)
    flags, first = load(paths)

    print("segment   code    data     gfx   sound  (words touched, by most specific use)")
    for seg in range(SPACE >> 16):
        c = [0, 0, 0, 0, 0]
        for v in flags[seg << 16:(seg + 1) << 16]:
            if v:
                c[[FETCH, DATA, PPU, SPU].index(kind(v))] += 1
        if any(c):
            print(f"  {seg:02X}    {c[0]:6d}  {c[1]:6d}  {c[2]:6d}  {c[3]:6d}")
    upper = sum(1 for v in flags[0x200000:] if v)
    print(f"upper half (0x200000+): {upper} words read")
    lo_dup = sum(1 for v in flags[0x100000:0x104000] if v)
    print(f"0x100000-0x103FFF (the high copy of the RAM/IO-shadowed block): {lo_dup} words read")

    if show_runs:
        print("\nstart    end      words  kind   first vblank")
        for s, e, k, f0 in runs(flags, first, gap):
            print(f"{s:06X}   {e:06X}  {e - s + 1:6d}  {NAMES[k]:5s}  {f0}")
    if out_png:
        png(out_png, flags)


if __name__ == "__main__":
    main()
