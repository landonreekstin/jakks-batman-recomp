#!/usr/bin/env python3
"""Decode the game's graphics from a video-state dump (`batman-rt --vdump`) and the ROM.

    gfx.py screen  <rom> <vNNNNN.bin> <out.png>   recompose the visible screen from pages + sprites
    gfx.py layers  <rom> <vNNNNN.bin> <outdir>    page1/page2 full 512x256 maps, sprites sheet,
                                                  palette, and report.txt (registers, sprite list,
                                                  ROM addresses of every tile in use)
    gfx.py sheet   <rom> <vNNNNN.bin> <word-addr> <count> <out.png> [bpp] [w] [h] [palbank]
                                                  raw tiles from ROM, coloured with that dump's palette

Formats (from recomp/src/ppu.c, a port of MAME's spg_renderer):
  - tile pixels: words read in order, each byte-swapped, then nc_bpp bits at a time from the top;
    tile n of a layer starts at word gfx_seg*0x40 + n * words_per_tile (this game: gfx_seg = 0)
  - colour: palette[(palette_bank << 4 >> bpp << bpp) + pixel], RGB555; bit 15 set = transparent
  - pages: 64x32 tilemap of tile numbers in RAM, plus one attribute byte per tile
    (bits 0-3 palette bank, 4 flip x, 5 flip y, 6 blend) when page ctrl bit 1 is clear
  - sprites: 256 x 4 words at 0x2C00 (tile, x, y, attr); attr as the page attribute register
    plus 0x4000 blend. With 0x2842 bit 1 clear the coordinates are centred.
`screen` reproduces what ppu_render draws (tilemap pages without row scroll / vertical
compression / linemap modes, which this game hasn't been seen to use), so comparing it with
the runtime's snapshot of the same vblank checks this decoder."""
import os
import struct
import sys
import zlib

BLEND_LEVELS = [0x08, 0x10, 0x18, 0x20]
EMPTY = 0x8000


def c5(v):
    return (v << 3) | (v >> 2)


def rgb(c):
    return (c5((c >> 10) & 31), c5((c >> 5) & 31), c5(c & 31))


def write_png(path, w, h, pixels):
    """pixels: list of rows of (r, g, b)"""
    raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in pixels)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Video:
    def __init__(self, rom_path, dump_path):
        data = open(rom_path, "rb").read()
        self.rom = struct.unpack(f"<{len(data) // 2}H", data)
        d = open(dump_path, "rb").read()
        self.vregs = struct.unpack_from("<256H", d, 0)
        self.vram = struct.unpack_from("<1792H", d, 512)
        self.ram = struct.unpack_from("<10240H", d, 512 + 3584)
        self.palette = self.vram[0x200:0x300]
        self.sprites = self.vram[0x300:0x700]

    def rd(self, a):
        a &= 0x3FFFFF
        if a < 0x2800:
            return self.ram[a]
        if 0x2900 <= a < 0x3000:
            return self.vram[a - 0x2900]
        return self.rom[a & 0x1FFFFF]

    def page(self, n):
        b = 0x10 if n == 1 else 0x16
        v = self.vregs
        return dict(xscroll=v[b], yscroll=v[b + 1], attr=v[b + 2], ctrl=v[b + 3], map=v[b + 4],
                    exattr=v[b + 5], seg=v[0x20 if n == 1 else 0x21])

    def tile_pixels(self, gfx_addr, tile, bpp, w, h):
        """rows of pixel values (palette-relative) for one tile"""
        bits_per_row = bpp * w // 16
        base = gfx_addr + bits_per_row * h * tile
        rows = []
        for y in range(h):
            m = base + bits_per_row * y
            row = []
            bits = nbits = 0
            for _ in range(w):
                bits <<= bpp
                if nbits < bpp:
                    b = self.rd(m)
                    m += 1
                    b = ((b << 8) | (b >> 8)) & 0xFFFF
                    bits |= b << (bpp - nbits)
                    nbits += 16
                nbits -= bpp
                row.append(bits >> 16)
                bits &= 0xFFFF
            rows.append(row)
        return rows

    @staticmethod
    def geometry(attr):
        bpp = ((attr & 3) + 1) << 1
        return bpp, 8 << ((attr & 0x30) >> 4), 8 << ((attr & 0xC0) >> 6)

    def page_tiles(self, n):
        """yields (tx, ty, tile, attr, ctrl) for the whole 512x256 map"""
        p = self.page(n)
        attr, ctrl = p["attr"], p["ctrl"]
        bpp, tw, th = self.geometry(attr)
        cols, rows = 512 // tw, 256 // th
        for ty in range(rows):
            for tx in range(cols):
                addr = tx + cols * ty
                tile = self.rd(p["map"]) if ctrl & 4 else self.rd(p["map"] + addr)
                tattr, tctrl = attr, ctrl
                if not ctrl & 2:
                    ex = self.rd(p["exattr"]) if ctrl & 4 else self.rd(p["exattr"] + addr // 2)
                    ex = (ex >> 8) if tx & 1 else (ex & 0xFF)
                    tattr = (tattr & ~0x000C) | ((ex >> 2) & 0x000C)
                    tattr = (tattr & ~0x0F00) | ((ex << 8) & 0x0F00)
                    tctrl = (tctrl & ~0x0100) | ((ex << 2) & 0x0100)
                yield tx, ty, tile, tattr, tctrl


def pal_offset(attr, bpp):
    return (((attr & 0x0F00) >> 4) >> bpp) << bpp


def draw_tile(buf, bw, bh, video, gfx_addr, tile, attr, x0, y0, blend, wrap_x=None, wrap_y=None):
    bpp, tw, th = Video.geometry(attr)
    pix = video.tile_pixels(gfx_addr, tile, bpp, tw, th)
    po = pal_offset(attr, bpp)
    fx, fy = attr & 4, attr & 8
    level = BLEND_LEVELS[video.vregs[0x2A] & 3]
    for y in range(th):
        row = pix[th - 1 - y if fy else y]
        yy = y0 + y
        if wrap_y:
            yy &= wrap_y
        if not 0 <= yy < bh:
            continue
        for x in range(tw):
            xx = x0 + x
            if wrap_x:
                xx &= wrap_x
            if not 0 <= xx < bw:
                continue
            c = video.palette[(po + row[tw - 1 - x if fx else x]) & 0xFF]
            if c & 0x8000:
                continue
            cur = buf[yy][xx]
            if blend and not cur & 0x8000:
                c = (((((0x20 - level) * ((cur >> 10) & 31) + level * ((c >> 10) & 31)) >> 5) << 10)
                     | ((((0x20 - level) * ((cur >> 5) & 31) + level * ((c >> 5) & 31)) >> 5) << 5)
                     | (((0x20 - level) * (cur & 31) + level * (c & 31)) >> 5))
            buf[yy][xx] = c


def check_supported(video):
    for n in (1, 2):
        p = video.page(n)
        if p["ctrl"] & 0x08 and p["ctrl"] & (0x01 | 0x10 | 0x40 | 0x80):
            sys.exit(f"page {n} ctrl {p['ctrl']:04x}: linemap/row scroll/compression not handled here")


def sprite_list(video):
    v = video.vregs
    out = []
    if not v[0x42] & 1:
        return out
    for n in range(256):
        tile, x, y, attr = video.sprites[4 * n:4 * n + 4]
        if not tile:
            continue
        x = x - 0x10000 if x & 0x8000 else x
        y = y - 0x10000 if y & 0x8000 else y
        bpp, tw, th = Video.geometry(attr)
        if not v[0x42] & 2:
            x, y = 160 + x - tw // 2, 128 - y - th // 2
        out.append(dict(n=n, tile=tile, x=x & 0x1FF, y=y & 0x1FF, attr=attr, bpp=bpp, w=tw, h=th,
                        addr=0x40 * v[0x22] + tile * bpp * tw * th // 16))
    return out


def screen(video):
    check_supported(video)
    buf = [[EMPTY] * 320 for _ in range(240)]
    pages = {n: video.page(n) for n in (1, 2)}
    sprites = sprite_list(video)
    for pri in range(4):
        for n in (1, 2):
            p = pages[n]
            if not p["ctrl"] & 0x08 or (p["attr"] & 0x3000) >> 12 != pri:
                continue
            bpp, tw, th = Video.geometry(p["attr"])
            for tx, ty, tile, tattr, tctrl in video.page_tiles(n):
                if tile:
                    draw_tile(buf, 320, 240, video, p["seg"] * 0x40, tile, tattr,
                              (tx * tw - p["xscroll"]) & 511, (ty * th - p["yscroll"]) & 255,
                              tctrl & 0x100, wrap_x=511, wrap_y=255)
        for s in sprites:
            if (s["attr"] & 0x3000) >> 12 == pri:
                # the hardware clips sprites against 512 lines with wrap; screen-space wrap is enough here
                draw_tile(buf, 320, 240, video, 0x40 * video.vregs[0x22], s["tile"], s["attr"],
                          s["x"], s["y"], s["attr"] & 0x4000, wrap_x=511, wrap_y=511)
    return [[(0, 0, 0) if c & 0x8000 else rgb(c) for c in row] for row in buf]


def layers(video, outdir):
    check_supported(video)
    os.makedirs(outdir, exist_ok=True)
    report = []
    v = video.vregs
    report.append(f"0x2842 sprite ctrl {v[0x42]:04x}  0x282a blend {v[0x2a]:04x}  fade {v[0x30]:04x}  "
                  f"sat {v[0x3c]:04x}  gfx seg page1/page2/sprites {v[0x20]:04x} {v[0x21]:04x} {v[0x22]:04x}")
    for n in (1, 2):
        p = video.page(n)
        bpp, tw, th = Video.geometry(p["attr"])
        report.append(f"page{n}: attr {p['attr']:04x} ctrl {p['ctrl']:04x} scroll {p['xscroll']:04x},{p['yscroll']:04x} "
                      f"tilemap RAM {p['map']:04x} attrs RAM {p['exattr']:04x}  {bpp}bpp {tw}x{th} "
                      f"priority {(p['attr'] >> 12) & 3}{'' if p['ctrl'] & 8 else '  (disabled)'}")
        buf = [[EMPTY] * 512 for _ in range(256)]
        used = {}
        for tx, ty, tile, tattr, tctrl in video.page_tiles(n):
            if tile:
                used.setdefault(tile, 0)
                used[tile] += 1
                draw_tile(buf, 512, 256, video, p["seg"] * 0x40, tile, tattr, tx * tw, ty * th, 0)
        img = [[(255, 0, 255) if c & 0x8000 else rgb(c) for c in row] for row in buf]
        # outline the visible window
        x0, y0 = p["xscroll"] & 511, p["yscroll"] & 255
        for i in range(320):
            for yy in (y0, (y0 + 239) & 255):
                img[yy][(x0 + i) & 511] = (255, 255, 255)
        for j in range(240):
            for xx in (x0, (x0 + 319) & 511):
                img[(y0 + j) & 255][xx & 511] = (255, 255, 255)
        write_png(os.path.join(outdir, f"page{n}.png"), 512, 256, img)
        wpt = bpp * tw * th // 16
        if used:
            addrs = sorted(p["seg"] * 0x40 + t * wpt for t in used)
            report.append(f"  {len(used)} distinct tiles, ROM words {addrs[0]:06X}-{addrs[-1] + wpt - 1:06X}")
            report.append("  regions (tile runs): " + ", ".join(
                f"{a:06X}+{n * wpt:X}" for a, n in runs(addrs, wpt)))

    sprites = sprite_list(video)
    report.append(f"sprites: {len(sprites)} active")
    for s in sprites:
        report.append(f"  #{s['n']:3d} tile {s['tile']:04x} -> ROM {s['addr']:06X}  {s['w']}x{s['h']} {s['bpp']}bpp "
                      f"pos {s['x']},{s['y']} attr {s['attr']:04x} pri {(s['attr'] >> 12) & 3}")
    # sprite sheet: each sprite on its own, in a grid of 64x64 cells
    cols = 8
    rows = max(1, (len(sprites) + cols - 1) // cols)
    buf = [[EMPTY] * (cols * 66) for _ in range(rows * 66)]
    for i, s in enumerate(sprites):
        draw_tile(buf, cols * 66, rows * 66, video, 0x40 * v[0x22], s["tile"], s["attr"] & ~0x3000,
                  (i % cols) * 66, (i // cols) * 66, 0)
    write_png(os.path.join(outdir, "sprites.png"), cols * 66, rows * 66,
              [[(48, 48, 56) if c & 0x8000 else rgb(c) for c in row] for row in buf])
    # palette: 16 banks of 16, 12x12 swatches, transparent entries crossed
    img = []
    for y in range(16 * 12):
        row = []
        for x in range(16 * 12):
            c = video.palette[(y // 12) * 16 + x // 12]
            if c & 0x8000:
                row.append((255, 0, 255) if (x % 12) == (y % 12) else (40, 40, 40))
            else:
                row.append(rgb(c))
        img.append(row)
    write_png(os.path.join(outdir, "palette.png"), 192, 192, img)
    with open(os.path.join(outdir, "report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report[:6]))


def runs(addrs, step):
    out = []
    for a in addrs:
        if out and a == out[-1][0] + out[-1][1] * step:
            out[-1][1] += 1
        else:
            out.append([a, 1])
    return out


def sheet(video, addr, count, out, bpp=4, w=8, h=8, palbank=0):
    attr = {2: 0, 4: 1, 6: 2, 8: 3}[bpp] | ({8: 0, 16: 1, 32: 2, 64: 3}[w] << 4) | ({8: 0, 16: 1, 32: 2, 64: 3}[h] << 6)
    attr |= (palbank & 15) << 8
    wpt = bpp * w * h // 16
    cols = max(1, 512 // w)
    rows = (count + cols - 1) // cols
    buf = [[EMPTY] * (cols * w) for _ in range(rows * h)]
    for i in range(count):
        # draw_tile takes a tile number relative to gfx_addr: use the address as base, tile index i
        draw_tile(buf, cols * w, rows * h, video, addr, i, attr, (i % cols) * w, (i // cols) * h, 0)
    write_png(out, cols * w, rows * h, [[(48, 48, 56) if c & 0x8000 else rgb(c) for c in row] for row in buf])


def main():
    a = sys.argv[1:]
    if len(a) < 4 or a[0] not in ("screen", "layers", "sheet"):
        print(__doc__)
        return 2
    video = Video(a[1], a[2])
    if a[0] == "screen":
        write_png(a[3], 320, 240, screen(video))
    elif a[0] == "layers":
        layers(video, a[3])
    else:
        extra = [int(x) for x in a[6:]]
        sheet(video, int(a[3], 16), int(a[4], 0), a[5], *extra)
    return 0


if __name__ == "__main__":
    sys.exit(main())
