#!/usr/bin/env python3
"""The player character's sprite frames (format in notes/sprites.md).

    sprites.py frames <rom> <out.png> [mod.txt]   every frame assembled, labelled with its index,
                                                  in the ROM palette (or with a mod's words applied)
    sprites.py tiles  <rom> <out.png> [mod.txt]   every 16x16 tile the frames use, 16 per row,
                                                  4x zoom, with the tile number above each one
    sprites.py export <rom> <out.png> [mod.txt]   the same tiles at 1x and in palette order, for
                                                  editing: an indexed-colour sheet whose pixels are
                                                  the 16 palette entries
    sprites.py import <rom> <in.png> [name]       prints a mod.txt with `words` lines for the tiles
                                                  that differ from the ROM (in.png as written by
                                                  export, edited with any of its 16 colours)

Batman's sprite bank starts at 0x051C40; offsets inside it are relative to that.
  - 0x054B4B: 183 frame offsets.
  - A frame is: count, flags, ?, k, then k box words, then 3 words, then count pieces of
    (dx, dy, tile index). dx/dy step from the previous piece (y down); the first is from the
    frame origin. Checked against the sprite list of a --vdump. Tile number = index + 0x400, a 16x16 4bpp tile at ROM word tile * 64.
  - The palette is bank 14, 16 words at 0x051870."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from gfx import write_png, rgb  # noqa: E402

BANK = 0x051C40
FRAME_LIST = 0x054B4B
NFRAMES = 183
TILE_BASE = 0x400
PALETTE = 0x051870
SHA = "4ec5438d16e222cc624820f45579071b8f8a4e91c65ae76e5425565509271766"


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def load_rom(path, mod=None):
    d = open(path, "rb").read()
    rom = list(struct.unpack(f"<{len(d) // 2}H", d))
    if mod:  # apply a mod's `words` lines (the only kind these tools write)
        for line in open(mod):
            p = line.split("#")[0].split()
            if p and p[0] == "words":
                a = int(p[1], 16)
                for i, w in enumerate(p[2:]):
                    rom[a + i] = int(w, 16)
    return rom


def record(rom, lst, f):
    """frame f of the frame list at word `lst`: dict with the header words, boxes, tail (effect id,
    spark x, spark y) and pieces [(x, y, tile)] (tile centres, y down, relative to the origin)"""
    a = BANK + rom[lst + f]
    n, k = rom[a], rom[a + 3]
    boxes = [tuple(s16(v) for v in rom[b:b + 5]) for b in range(a + 4, a + 4 + k, 5)]
    p = a + 7 + k
    x = y = 0
    pieces = []
    for i in range(n):
        dx, dy, t = rom[p + 3 * i:p + 3 * i + 3]
        x += s16(dx)
        y += s16(dy)
        pieces.append((x, y, t + TILE_BASE))
    return {"addr": a, "flags": rom[a + 1], "word2": rom[a + 2], "boxes": boxes,
            "tail": tuple(rom[a + 4 + k:a + 7 + k]), "pieces": pieces}


def encode_record(r):
    """words of a frame record (inverse of record)"""
    k = 5 * len(r["boxes"])
    w = [len(r["pieces"]), r["flags"], r["word2"], k]
    for b in r["boxes"]:
        w += [v & 0xFFFF for v in b]
    w += list(r["tail"])
    px = py = 0
    for x, y, t in r["pieces"]:
        w += [(x - px) & 0xFFFF, (y - py) & 0xFFFF, (t - TILE_BASE) & 0xFFFF]
        px, py = x, y
    return w


def frames(rom):
    """list of frames; a frame is a list of (x, y, tile) with y down, relative to the origin"""
    out = []
    for f in range(NFRAMES):
        a = BANK + rom[FRAME_LIST + f]
        n, k = rom[a], rom[a + 3]
        p = a + 7 + k
        x = y = 0
        pieces = []
        for i in range(n):
            dx, dy, t = rom[p + 3 * i:p + 3 * i + 3]
            x += s16(dx)
            y += s16(dy)
            pieces.append((x, y, t + TILE_BASE))
        out.append(pieces)
    return out


def tile_pixels(rom, tile):
    """16x16 palette indices; words byte-swapped, nibbles from the top (see notes/graphics.md)"""
    base = tile * 64
    rows = []
    for y in range(16):
        row = []
        for wi in range(4):
            w = rom[base + y * 4 + wi]
            w = ((w << 8) | (w >> 8)) & 0xFFFF
            row += [(w >> s) & 15 for s in (12, 8, 4, 0)]
        rows.append(row)
    return rows


def pixels_to_words(rows):
    words = []
    for y in range(16):
        for wi in range(4):
            v = 0
            for p in rows[y][wi * 4:wi * 4 + 4]:
                v = (v << 4) | p
            words.append(((v << 8) | (v >> 8)) & 0xFFFF)
    return words


def palette(rom):
    return [rgb(rom[PALETTE + i]) for i in range(16)]


GLYPHS = {  # 3x5 digits and hex letters for labels
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
    "8": "111101111101111", "9": "111101111001111", "a": "010101111101101", "b": "110101110101110",
    "c": "011100100100011", "d": "110101101101110", "e": "111100110100111", "f": "111100110100100",
}


def label(img, x, y, text, colour=(255, 255, 255)):
    for i, ch in enumerate(text):
        g = GLYPHS[ch]
        for k in range(15):
            if g[k] == "1":
                yy, xx = y + k // 3, x + i * 4 + k % 3
                if 0 <= yy < len(img) and 0 <= xx < len(img[0]):
                    img[yy][xx] = colour


def cmd_frames(rom, out):
    fs = frames(rom)
    pal = palette(rom)
    cell, cols = 96, 16
    rows_n = (len(fs) + cols - 1) // cols
    img = [[(40, 40, 48)] * (cell * cols) for _ in range(cell * rows_n)]
    for f, pieces in enumerate(fs):
        ox, oy = (f % cols) * cell + cell // 2, (f // cols) * cell + cell // 2 + 10
        for x, y, t in pieces:
            px = tile_pixels(rom, t)
            for ty in range(16):
                for tx in range(16):
                    c = px[ty][tx]
                    # sprite x/y are the tile centre (centred coordinates, notes/graphics.md)
                    X, Y = ox + x - 8 + tx, oy + y - 8 + ty
                    if c and 0 <= X < len(img[0]) and 0 <= Y < len(img):
                        img[Y][X] = pal[c]
        label(img, (f % cols) * cell + 2, (f // cols) * cell + 2, f"{f}")
        img[oy][ox] = (255, 0, 255)
    write_png(out, len(img[0]), len(img), img)
    print(f"{out}: {len(fs)} frames")


def used_tiles(rom):
    return sorted({t for pieces in frames(rom) for _, _, t in pieces})


def cmd_tiles(rom, out, zoom=4):
    ts = used_tiles(rom)
    pal = palette(rom)
    cols = 16
    cw = 16 * zoom + 4
    ch = 16 * zoom + 10
    img = [[(40, 40, 48)] * (cw * cols) for _ in range(ch * ((len(ts) + cols - 1) // cols))]
    for i, t in enumerate(ts):
        bx, by = (i % cols) * cw, (i // cols) * ch
        label(img, bx + 2, by + 2, f"{t:x}")
        px = tile_pixels(rom, t)
        for y in range(16 * zoom):
            for x in range(16 * zoom):
                c = px[y // zoom][x // zoom]
                img[by + 8 + y][bx + 2 + x] = pal[c] if c else (90, 90, 100)
    write_png(out, len(img[0]), len(img), img)
    print(f"{out}: {len(ts)} tiles")


def write_indexed_png(path, w, h, rows, pal):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    plte = b"".join(bytes(c) for c in pal)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
                + chunk(b"PLTE", plte) + chunk(b"tRNS", b"\0")
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_png_any(path):
    """(w, h, rows of palette indices or RGB tuples, palette or None); 8-bit, no interlace"""
    data = open(path, "rb").read()
    pos, idat, plte = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack_from(">IIBB", body)
            if depth != 8 or ctype not in (2, 3, 6):
                sys.exit(f"{path}: needs an 8-bit indexed or RGB(A) PNG")
            bpp = {2: 3, 3: 1, 6: 4}[ctype]
        elif typ == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        ft, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 3:
            rows.append(list(line))
        else:
            rows.append([tuple(line[i:i + 3]) for i in range(0, stride, bpp)])
    return w, h, rows, (plte if ctype == 3 else None)


def cmd_export(rom, out):
    """tiles 16 per row, 1 px gaps (index 0), as an indexed PNG in the character's palette"""
    ts = used_tiles(rom)
    cols = 16
    W, H = cols * 17, ((len(ts) + cols - 1) // cols) * 17
    img = [[0] * W for _ in range(H)]
    for i, t in enumerate(ts):
        px = tile_pixels(rom, t)
        for y in range(16):
            img[(i // cols) * 17 + y][(i % cols) * 17:(i % cols) * 17 + 16] = px[y]
    pal = palette(rom)
    pal[0] = (255, 0, 255)  # index 0 is transparent in the game
    write_indexed_png(out, W, H, img, pal)
    print(f"{out}: {len(ts)} tiles in order {ts[0]:04x}..{ts[-1]:04x}; edit with the 16 palette "
          "colours only, then `sprites.py import`")


def cmd_import(rom, path, name=None):
    ts = used_tiles(rom)
    w, h, rows, plte = read_png_any(path)
    pal = palette(rom)
    pal[0] = (255, 0, 255)
    if plte is None:  # RGB: map each colour to the palette entry it equals
        lut = {c: i for i, c in reversed(list(enumerate(pal)))}
        try:
            rows = [[lut[c] for c in r] for r in rows]
        except KeyError as e:
            sys.exit(f"{path}: colour {e} is not one of the character's 16 colours")
    print(f"name {name or 'sprite edit'}")
    print(f"rom  {SHA}")
    print("# made with tools/sprites.py import")
    changed = 0
    for i, t in enumerate(ts):
        px = [rows[(i // 16) * 17 + y][(i % 16) * 17:(i % 16) * 17 + 16] for y in range(16)]
        if any(v > 15 for r in px for v in r):
            sys.exit(f"{path}: tile {t:04x} uses a colour index above 15")
        words = pixels_to_words(px)
        old = rom[t * 64:t * 64 + 64]
        if words != list(old):
            changed += 1
            for k in range(0, 64, 16):
                print(f"words {t * 64 + k:06x} " + " ".join(f"{v:04x}" for v in words[k:k + 16]))
    print(f"# {changed} tiles changed", file=sys.stderr)


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return 2
    if a[0] == "import":
        cmd_import(load_rom(a[1]), a[2], a[3] if len(a) > 3 else None)
        return 0
    rom = load_rom(a[1], a[3] if len(a) > 3 else None)
    {"frames": cmd_frames, "tiles": cmd_tiles, "export": cmd_export}[a[0]](rom, a[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
