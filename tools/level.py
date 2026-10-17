#!/usr/bin/env python3
"""Background maps of the game's scenes (format in notes/level-format.md).

    level.py list   <rom>                          the scene table, with map sizes
    level.py render <rom> <scene> <page> <out.png> [vNNNNN.bin]
                                                   whole map of page 1 or 2 as one image, coloured
                                                   with the scene's ROM palette (or a --vdump's)
    level.py check  <rom> <scene> <page> <vNNNNN.bin> ...
                                                   compares the decoded map with the tilemap and
                                                   attribute RAM in video-state dumps, tile by tile
    level.py extract <rom> <scene> <out.json>      both maps as block grids (hex bytes, one row per
                                                   string), their block tables and the scene's
                                                   other data blocks (raw words, hex)
    level.py build  <rom> <in.json> <out.bin>      re-encodes a scene's data block from JSON
                                                   (extract + build reproduces the ROM words exactly)
    level.py paste  <in.json> <page> <src-col> <dst-col> <count> <out.json>
                                                   copies <count> block columns of a map (all rows)
    level.py diff   <rom> <in.json> [name]         prints a mod.txt with `words` lines for the words
                                                   that differ from the ROM (the data block must keep
                                                   its size)

Scene table at 0x0D869E: a count, then 24-word records of 12 far pointers (low word, segment):
  0 tileset graphics (tile number base = address / 16)   1 background palette (banks 0-7)
  2 page 2 map   3 page 1 map   4-11 not decoded yet
A map is: width, height (in 32x32 blocks), n, then n block-table entries, then height rows of
width/2 words, two block bytes per word (low byte = even column). Byte b selects table entry b, an
offset into the global block pools: tiles at 0x09EC10 + e (4x4 tile numbers, row-major, plus the
tile base), attributes at 0x0A6580 + e/2 (4 rows of 2 words, one attribute byte per tile, low byte =
even column)."""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from gfx import Video, draw_tile, write_png, rgb  # noqa: E402

SCENES = 0x0D869E
REC_WORDS = 0x18
TILE_POOL = 0x09EC10
ATTR_POOL = 0x0A6580
SLOTS = ["tileset", "palette", "page2", "page1"] + [f"data{i}" for i in range(4, 12)]
PAGE_SLOT = {1: 3, 2: 2}


def load_rom(path):
    d = open(path, "rb").read()
    return struct.unpack(f"<{len(d) // 2}H", d)


def scene_ptrs(rom, n):
    r = rom[SCENES + 1 + n * REC_WORDS:SCENES + 1 + (n + 1) * REC_WORDS]
    return [r[k] | r[k + 1] << 16 for k in range(0, REC_WORDS, 2)]


class Map:
    def __init__(self, rom, addr, tile_base):
        self.addr = addr
        self.w, self.h, n = rom[addr], rom[addr + 1], rom[addr + 2]
        self.table = list(rom[addr + 3:addr + 3 + n])
        g = addr + 3 + n
        stride = self.w // 2
        self.grid = [[(rom[g + y * stride + x // 2] >> (0 if x % 2 == 0 else 8)) & 0xFF
                      for x in range(self.w)] for y in range(self.h)]
        self.words = 3 + n + stride * self.h
        self.rom = rom
        self.tile_base = tile_base
        self.grid_addr, self.stride = g, stride

    def entry(self, bx, by):
        """read the way the game does (FUN_00c3e4): rows past the map read whatever follows it"""
        w = self.rom[self.grid_addr + by * self.stride + bx // 2]
        return self.rom[self.addr + 3 + ((w >> 8 if bx & 1 else w) & 0xFF)]

    def tile(self, tx, ty):
        """(tile number, attribute byte) at tile column tx, row ty, as the game writes them"""
        e = self.entry(tx // 4, ty // 4)
        t = (self.rom[TILE_POOL + e + (ty % 4) * 4 + tx % 4] + self.tile_base) & 0xFFFF
        a = self.rom[ATTR_POOL + e // 2 + (ty % 4) * 2 + (tx % 4) // 2]
        return t, (a >> 8 if tx & 1 else a) & 0xFF


def scene_map(rom, scene, page):
    p = scene_ptrs(rom, scene)
    if not p[PAGE_SLOT[page]]:
        sys.exit(f"scene {scene} has no page {page} map")
    return Map(rom, p[PAGE_SLOT[page]], p[0] // 16)


def cmd_list(rom):
    print(f"{rom[SCENES]} scenes at {SCENES:06X}")
    for s in range(rom[SCENES]):
        p = scene_ptrs(rom, s)
        desc = []
        for page in (2, 1):
            a = p[PAGE_SLOT[page]]
            if a:
                m = Map(rom, a, 0)
                desc.append(f"page{page} {m.w}x{m.h} blocks, {len(m.table)} kinds")
        print(f"{s:2d}  tiles {p[0]:06X} palette {p[1]:06X}  " + ";  ".join(desc))


class RomPalette:
    """enough of gfx.Video for draw_tile, with the palette from the scene (or a dump)"""
    def __init__(self, rom, pal_addr, dump=None):
        self.rom = rom
        if dump:
            self.palette = Video.__new__(Video)
            d = open(dump, "rb").read()
            self.palette = struct.unpack_from("<1792H", d, 512)[0x200:0x300]
        else:
            pal = [rom[pal_addr + i] for i in range(128)] + [0x8000] * 128
            for b in range(16):
                pal[b * 16] |= 0x8000  # colour 0 is transparent (the game sets bit 15)
            self.palette = pal
        self.vregs = [0] * 256
        self._cache = {}

    def rd(self, a):
        return self.rom[a & 0x1FFFFF]

    def tile_pixels(self, gfx_addr, tile, bpp, w, h):
        k = (tile, bpp, w, h)
        if k not in self._cache:
            self._cache[k] = Video.tile_pixels(self, gfx_addr, tile, bpp, w, h)
        return self._cache[k]


def cmd_render(rom, scene, page, out, dump=None):
    m = scene_map(rom, scene, page)
    v = RomPalette(rom, scene_ptrs(rom, scene)[1], dump)
    W, H = m.w * 32, m.h * 32
    buf = [[0x8000] * W for _ in range(H)]
    for ty in range(m.h * 4):
        for tx in range(m.w * 4):
            t, a = m.tile(tx, ty)
            if not t:
                continue
            attr = 0x0001 | ((a & 0x0F) << 8) | ((a >> 2) & 0x0C)  # 4bpp 8x8, bank, flips
            draw_tile(buf, W, H, v, 0, t, attr, tx * 8, ty * 8, False)
    write_png(out, W, H, [[(0, 0, 0) if c & 0x8000 else rgb(c) for c in row] for row in buf])
    print(f"{out}: scene {scene} page {page}, {m.w}x{m.h} blocks = {W}x{H} px")


def cmd_check(rom, scene, page, dumps):
    """For each dump: every tile column on screen (from the camera) must equal a column of the
    decoded map, tiles and attributes, on every visible row. The column is first looked up the way
    the game does it (brawler section loop at 0x270B); columns written under an earlier section
    state are then searched for in the whole map. Returns the number of columns found nowhere."""
    m = scene_map(rom, scene, page)
    ncols = m.w * 4
    cols = {}
    for c in range(ncols):  # (tile, attr) per row, plus rows 32-35 that the game reads past the map
        cols[c] = [m.tile(c, ty) for ty in range(m.h * 4 + 4)]
    tot = {"direct": 0, "elsewhere": 0, "none": 0}
    for path in dumps:
        v = Video.__new__(Video)
        d = open(path, "rb").read()
        v.vregs = struct.unpack_from("<256H", d, 0)
        v.ram = struct.unpack_from("<10240H", d, 512 + 3584)
        p = Video.page(v, page)
        # The scroll registers hold the position modulo the 512x256 ring; the level position is
        # the camera (0x208E/0x208F), which the game copies into page 2's scroll a frame later.
        # Page 1 scrolls at half speed and wraps around its own map width.
        cam_x, cam_y = v.ram[0x208E], v.ram[0x208F]
        if page == 2:
            lx = cam_x - ((cam_x - p["xscroll"]) & 511)
            ly = cam_y - ((cam_y - p["yscroll"]) & 255)
        else:
            lx, ly = p["xscroll"], p["yscroll"]
        x0, y0 = lx // 8, ly // 8
        rows = range(y0, min(y0 + 31, m.h * 4))
        sec = v.ram[0x270B]  # brawler section loop (FUN_00e74e), page 2 only

        def remap(tx):
            if page != 2 or not sec or tx < v.ram[sec + 1]:
                return tx % ncols
            return ((tx - v.ram[sec + 1]) % v.ram[sec] + v.ram[sec + 7]) % ncols

        def ram_col(tx):
            out = []
            for ty in rows:
                cell = (tx % 64) + 64 * (ty % 32)
                ra = v.ram[p["exattr"] + cell // 2]
                out.append((v.ram[p["map"] + cell], (ra >> 8 if tx & 1 else ra) & 0xFF))
            return out

        def fits(c, got):
            # rows 0-3 of the ring may still hold rows 32-35, written while the camera was lower
            return all(g == cols[c][ty] or (ty < 4 and g == cols[c][ty + 32]) for ty, g in zip(rows, got))
        res = {"direct": 0, "elsewhere": 0, "none": 0}
        for tx in range(x0, x0 + 41):
            if page == 2 and tx >= (lx + 320 + 7) // 8 + 1:
                break
            got = ram_col(tx)
            if fits(remap(tx), got):
                res["direct"] += 1
            elif any(fits(c, got) for c in cols if c % 2 == tx % 2):
                res["elsewhere"] += 1
            else:
                res["none"] += 1
                if res["none"] <= 3:
                    print(f"  {os.path.basename(path)} column {tx}: no map column matches")
        for k in tot:
            tot[k] += res[k]
        print(f"{os.path.basename(path)}: camera {cam_x},{cam_y} scroll {p['xscroll']},{p['yscroll']}  "
              f"{res['direct']} columns as the game maps them, {res['elsewhere']} found elsewhere, "
              f"{res['none']} unmatched")
    print(f"total: {tot['direct']} direct, {tot['elsewhere']} elsewhere, {tot['none']} unmatched")
    return tot["none"]


def scene_block(rom, scene):
    """(start, end) of the scene's contiguous data: palette .. last data block"""
    p = scene_ptrs(rom, scene)
    start = p[1]
    # the last scene's data ends where the scene table starts
    nxt = scene_ptrs(rom, scene + 1)[1] if scene + 1 < rom[SCENES] else SCENES
    return start, nxt


def cmd_extract(rom, scene, out):
    p = scene_ptrs(rom, scene)
    j = {"scene": scene, "tileset": p[0], "pointers": {}, "maps": {}, "blocks": []}
    order = sorted((a, i) for i, a in enumerate(p) if i >= 1 and a)
    start, nxt = scene_block(rom, scene)
    for k, (a, i) in enumerate(order):
        end = order[k + 1][0] if k + 1 < len(order) else nxt
        j["pointers"][SLOTS[i]] = a
        if SLOTS[i] in ("page1", "page2"):
            m = Map(rom, a, 0)
            if a + m.words != end:
                j["blocks"].append({"slot": SLOTS[i] + "_tail", "addr": a + m.words,
                                    "words": hexrows(rom[a + m.words:end])})
            j["maps"][SLOTS[i]] = {"width": m.w, "height": m.h, "table": hexrows(m.table),
                                   "grid": [" ".join(f"{b:02x}" for b in row) for row in m.grid]}
        else:
            j["blocks"].append({"slot": SLOTS[i], "addr": a, "words": hexrows(rom[a:end])})
    j["start"], j["end"] = start, nxt
    with open(out, "w") as f:
        json.dump(j, f, indent=1)
    print(f"{out}: scene {scene}, data {start:06X}-{nxt - 1:06X} ({nxt - start} words)")


def hexrows(words, per=16):
    return [" ".join(f"{w:04x}" for w in words[k:k + per]) for k in range(0, len(words), per)]


def unhex(rows):
    return [int(w, 16) for r in rows for w in r.split()]


def encode_map(m):
    table = unhex(m["table"])
    words = [m["width"], m["height"], len(table)] + table
    for row in m["grid"]:
        b = [int(x, 16) for x in row.split()]
        if len(b) != m["width"]:
            sys.exit("grid row length != width")
        words += [b[x] | b[x + 1] << 8 for x in range(0, len(b), 2)]
    return words


def cmd_build(rom, path, out):
    j = json.load(open(path))
    words = encode_scene(j)
    with open(out, "wb") as f:
        f.write(struct.pack(f"<{len(words)}H", *words))
    same = list(rom[j["start"]:j["start"] + len(words)]) == words
    print(f"{out}: {len(words)} words for {j['start']:06X}; "
          f"{'identical to the ROM' if same else 'differs from the ROM'}")


def encode_scene(j):
    data = {}
    for slot, a in j["pointers"].items():
        if slot in j["maps"]:
            data[a] = encode_map(j["maps"][slot])
    for b in j["blocks"]:
        data[b["addr"]] = unhex(b["words"])
    words = []
    pos = j["start"]
    for a in sorted(data):
        if a != pos:
            sys.exit(f"gap or overlap at {a:06X} (expected {pos:06X})")
        words += data[a]
        pos += len(data[a])
    return words


def cmd_paste(path, page, src, dst, count, out):
    j = json.load(open(path))
    m = j["maps"][f"page{page}"]
    grid = [r.split() for r in m["grid"]]
    for row in grid:
        row[dst:dst + count] = row[src:src + count]
    m["grid"] = [" ".join(r) for r in grid]
    with open(out, "w") as f:
        json.dump(j, f, indent=1)
    print(f"{out}: page {page} block columns {src}-{src + count - 1} copied to {dst}-{dst + count - 1}")


def cmd_diff(rom, path, name=None):
    j = json.load(open(path))
    words = encode_scene(j)
    if j["start"] + len(words) != j["end"]:
        sys.exit(f"the scene's data changed size ({len(words)} words, was {j['end'] - j['start']}); "
                 "moving it needs the pointers patched too")
    sha = __import__("hashlib").sha256(open(sys.argv[2], "rb").read()).hexdigest()
    print(f"name {name or 'scene ' + str(j['scene']) + ' edit'}")
    print(f"rom  {sha}")
    print(f"# made with tools/level.py diff from an edited extract of scene {j['scene']}")
    k = 0
    while k < len(words):
        if words[k] == rom[j["start"] + k]:
            k += 1
            continue
        e = k
        while e < len(words) and e - k < 16 and words[e] != rom[j["start"] + e]:
            e += 1
        print(f"words {j['start'] + k:06x} " + " ".join(f"{w:04x}" for w in words[k:e]))
        k = e


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        return 2
    if a[0] == "paste":
        cmd_paste(a[1], int(a[2]), int(a[3]), int(a[4]), int(a[5]), a[6])
        return 0
    rom = load_rom(a[1])
    if a[0] == "list":
        cmd_list(rom)
    elif a[0] == "render":
        cmd_render(rom, int(a[2]), int(a[3]), a[4], a[5] if len(a) > 5 else None)
    elif a[0] == "check":
        return 1 if cmd_check(rom, int(a[2]), int(a[3]), a[4:]) else 0
    elif a[0] == "extract":
        cmd_extract(rom, int(a[2]), a[3])
    elif a[0] == "build":
        cmd_build(rom, a[2], a[3])
    elif a[0] == "diff":
        cmd_diff(rom, a[2], a[3] if len(a) > 3 else None)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
