#!/usr/bin/env python3
"""Generates mods/robin/mod.txt: Robin's palette and the edited sprite tiles.

    mods/robin/generate.py target/batman.bin > mods/robin/mod.txt   (make -C recomp does this)

The output holds edited copies of the original art, so like target/ it stays out of git.

The edits are rules applied to every assembled frame (tools/sprites.py). Each tile pixel is
then set to the result most of its uses agree on, because frames share tiles.
  - hair: cowl pixels (cape colours) next to the face (skin colours), at or above it, become
    black hair with dark-brown highlights. The cowl's ears become spikes of hair.
  - legs: suit pixels below the belt become green. Where no belt shows, the waist is taken as
    45% of the way up the body.
  - staff reach: B attacks in melee up to 40 + REACH px away instead of 40, and every attack box (frame box types other than 1/2, the body) that reaches forward
    gets REACH more pixels at its far edge, and its hit spark moves with it (notes/sprites.md).
  - palette: colours 1 and 5 were barely used (38 and 48 pixels), so they become the two greens.
    Their old pixels move to the neighbouring shade."""
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import sprites  # noqa: E402

CAPE = {1, 2, 3, 4}
SUIT = {5, 6, 7, 8, 9}
BELT = 10
SKIN = {11, 12, 13}
GREEN_LIGHT, GREEN_DARK = 1, 5
REACH = 20  # px added to forward attack boxes: Robin's staff
# FUN_0dada3 picks the enemy a B press attacks in melee: the nearest within 40 px across and
# 24 px in depth; with none, B throws the Batarang. Its `cmp r3, 0x28` (0x4668 at 0x0DADD5) holds
# the 40 in a 6-bit immediate, so the staff's range is 40 + REACH <= 63.
MELEE_RANGE_CMP = 0x0DADD5


def c(r, g, b):
    return r << 10 | g << 5 | b


PALETTE = {  # index: colour (RGB555); 0, 10-15 keep Batman's
    1: c(9, 22, 6),     # green light
    2: c(31, 29, 10),   # cape yellow, light
    3: c(29, 23, 3),    # cape yellow
    4: c(20, 14, 2),    # cape yellow, dark
    5: c(3, 12, 3),     # green dark
    6: c(31, 9, 7),     # red, light
    7: c(25, 5, 4),     # red
    8: c(19, 3, 3),     # red, dark
    9: c(12, 2, 2),     # red, darkest
}


def recolour(frame_pixels):
    """{(x, y): colour} for one assembled frame -> {(x, y): new colour}"""
    skin = [p for p, v in frame_pixels.items() if v in SKIN]
    belt = [p for p, v in frame_pixels.items() if v == BELT]
    if belt:
        belt_y = max(y for _, y in belt)
    else:  # no belt in view (side views, flips): the waist is ~45% up the body, cape excluded
        body = [y for (_, y), v in frame_pixels.items() if v in SUIT or v in SKIN]
        belt_y = max(body) - int(0.45 * (max(body) - min(body))) if len(body) > 40 else None
    out = {}
    for (x, y), v in frame_pixels.items():
        n = v
        if v in CAPE and any(abs(x - sx) <= 5 and -9 <= y - sy <= 2 for sx, sy in skin):
            n = 14 if v in (1, 2) else 15                     # hair
        elif v in SUIT and belt_y is not None and y > belt_y:
            n = GREEN_LIGHT if v in (5, 6, 7) else GREEN_DARK  # legs
        elif v == 1:
            n = 2
        elif v == 5:
            n = 6
        out[(x, y)] = n
    return out


def reach_words(rom):
    """`words` lines lengthening the forward attack boxes (box = x0, y0, x1, y1, type)"""
    out = []
    for f in range(sprites.NFRAMES):
        a = sprites.BANK + rom[sprites.FRAME_LIST + f]
        k = rom[a + 3]
        tail = a + 4 + k  # effect id, spark x, spark y
        moved = False
        for b in range(a + 4, a + 4 + k, 5):
            x1, typ = sprites.s16(rom[b + 2]), rom[b + 4]
            if typ not in (1, 2) and x1 > 0:
                out.append(f"words {b + 2:06x} {(x1 + REACH) & 0xFFFF:04x}   # frame {f} attack box x1")
                moved = True
        if moved and sprites.s16(rom[tail + 1]) > 0:
            out.append(f"words {tail + 1:06x} {(sprites.s16(rom[tail + 1]) + REACH) & 0xFFFF:04x}   # frame {f} hit spark x")
    return out


STAFF_ADDR = 0x3F0000  # tiles 0xFC00 (middle) and 0xFC01 (end), in the ROM's free upper half (plugin.c)


def staff_tiles():
    """two 16x16 tiles of a horizontal staff, tip to the right: outline 15, wood 12/13/14,
    a yellow (10) cap on the end tile"""
    bar = {6: 15, 7: 12, 8: 13, 9: 14, 10: 15}
    mid = [[bar.get(y, 0) for x in range(16)] for y in range(16)]
    end = [row[:] for row in mid]
    for y in range(6, 11):
        for x in range(11, 16):
            end[y][x] = 0
        for x in range(11, 14):
            end[y][x] = 15 if y in (6, 10) else 10
        end[y][14] = 15 if 7 <= y <= 9 else 0
    return mid, end


def main():
    rom = sprites.load_rom(sys.argv[1])
    votes = defaultdict(Counter)  # (tile, tx, ty) -> Counter of new colours
    for pieces in sprites.frames(rom):
        where = {}
        pix = {}
        for x, y, t in pieces:
            px = sprites.tile_pixels(rom, t)
            for ty in range(16):
                for tx in range(16):
                    if px[ty][tx]:
                        p = (x + tx, y + ty)
                        pix[p] = px[ty][tx]
                        where.setdefault(p, []).append((t, tx, ty))
        new = recolour(pix)
        for p, uses in where.items():
            for u in uses:
                votes[u][new[p]] += 1
    print("# generated by mods/robin/generate.py (do not edit): Robin's palette and sprite tiles")
    print("name robin")
    print(f"rom  {sprites.SHA}")
    print("words 051871 " + " ".join(f"{PALETTE[i]:04x}" for i in range(1, 10)))
    print("\n".join(reach_words(rom)))
    for i, t in enumerate(staff_tiles()):
        words = sprites.pixels_to_words(t)
        for k in range(0, 64, 16):
            print(f"words {STAFF_ADDR + 64 * i + k:06x} " + " ".join(f"{w:04x}" for w in words[k:k + 16]))
    print("plugin plugin.so   # draws the staff (plugin.c)")
    assert rom[MELEE_RANGE_CMP] == 0x4668 and 40 + REACH <= 63
    print(f"words {MELEE_RANGE_CMP:06x} {0x4640 | (40 + REACH):04x}   # melee range 40 -> {40 + REACH} px")
    changed = 0
    for t in sprites.used_tiles(rom):
        px = sprites.tile_pixels(rom, t)
        for ty in range(16):
            for tx in range(16):
                if px[ty][tx] and votes[(t, tx, ty)]:
                    px[ty][tx] = votes[(t, tx, ty)].most_common(1)[0][0]
                elif px[ty][tx] in (1, 5):  # unused by any frame: still move off the freed slots
                    px[ty][tx] = {1: 2, 5: 6}[px[ty][tx]]
        words = sprites.pixels_to_words(px)
        if words != list(rom[t * 64:t * 64 + 64]):
            changed += 1
            for k in range(0, 64, 16):
                print(f"words {t * 64 + k:06x} " + " ".join(f"{w:04x}" for w in words[k:k + 16]))
    print(f"# {changed} tiles changed", file=sys.stderr)


if __name__ == "__main__":
    main()
