#!/usr/bin/env python3
"""Generates mods/robin/mod.txt: Robin, built from the staff fighter's sprites.

    mods/robin/generate.py target/batman.bin > mods/robin/mod.txt   (make -C recomp does this)

The output holds edited copies of the original art, so like target/ it stays out of git.

How (formats in notes/sprites.md):
  - Each of the player's frames 0-138 (the ones the animation table at 0x5744 uses) is rebuilt
    from a frame of the staff fighter (frame list 0x05A2E5; slim, with a real staff and its own
    attack poses and boxes), or, for hurt/knockdown and the crouch (the fighter has none), the
    thug at 0x05F16C. ANIM_SOURCES says which.
  - Each source frame is assembled, repainted as Robin (robinize: red tunic, green legs and
    sleeves, a narrow cape along the back, and one drawn head, HEAD: spiky hair, a domino mask
    under a strip of forehead, a rounded jaw; upright over the body) and cut back into 16x16 pieces. The pieces keep the source layout,
    plus extra ones where the cape reaches outside it. Identical tiles are shared.
  - The tiles go where Batman's tiles were (those only frames 0-138 use: 443 of them), the frame
    records where Batman's records 0-138 were, and the frame list points at them. Frames 139-182
    (unused in Street Party and Fast Lane) are left alone.
  - Boxes: the source's body boxes; attack boxes from the source frame when it has them, else
    from the Batman frame in that slot (so every attack still hits on the same frames). Hero
    attacks keep their hit type 20.
  - Animations keep their frame slots (code may depend on the numbers); attacks play at the staff
    fighter's own speed (FPS_ATTACK, measured on screen in session1: 4-5 ticks a frame).
  - B attacks in melee up to 60 px away instead of 40 (the staff's reach; the fighter's swing
    boxes reach 61 px). The Birdarang is the Batarang recoloured, faster and further.
Everything is a ROM data change, so a baked ROM runs in MAME too."""
import os
import sys
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import sprites  # noqa: E402

FIGHTER, FIGHTER_PAL = 0x05A2E5, 0x051A00   # the yellow staff fighter (Street Party, 2nd half)
THUG, THUG_PAL = 0x05F16C, 0x051B50         # a thug with fall/lie/get-up frames
ANIMS = 0x5744                              # player animations: first, last, loop (1-based), fps
PLAYER_SLOTS = 139                          # frames 0-138
TILE_AREA_FRAMES = range(139)


def c(r, g, b):
    return r << 10 | g << 5 | b


# Robin's palette (sprite bank 14, 0x051870)
RED_L, RED, RED_D, GRN_L, GRN, GRN_D, YEL_L, YEL, BRN_D, SKIN_L, SKIN, SKIN_D, WHITE, WOOD, BLACK = range(1, 16)
PALETTE = {
    RED_L: c(31, 9, 7), RED: c(24, 4, 4), RED_D: c(15, 2, 2),
    GRN_L: c(10, 24, 7), GRN: c(4, 16, 4), GRN_D: c(2, 9, 2),
    YEL_L: c(31, 30, 12), YEL: c(30, 23, 3), BRN_D: c(15, 9, 3),
    SKIN_L: c(31, 25, 19), SKIN: c(27, 19, 13), SKIN_D: c(19, 12, 8),
    WHITE: c(31, 31, 31), WOOD: c(18, 11, 5), BLACK: 0,
}

# Robin's head, facing right, in the game's style (outlined, shaded at the back, about the
# fighter's head size): spiky black hair swept back, a strip of forehead, the domino mask with
# both eyes showing, a rounded jaw. K hair, S/s/d skin light/mid/dark, M mask,
# W eye.
HEAD = [
    ".K.K.K.",
    "KKKKKK.",
    "KKKSSS.",
    "KKMWMWK",
    "KKdssSS",
    ".KdsssK",
    ".Kddss.",
    "..Kdsd.",
]
HEAD_SHIFT = 1   # columns forward of the source head's centre
HEAD_COLOURS = {"K": BLACK, "S": SKIN_L, "s": SKIN, "d": SKIN_D, "M": BLACK, "W": WHITE}

# what each source palette index is: suit ramp (light..dark), skin ramp, staff, etc.
CLASSES = {
    # skin: seven browns, light to dark (6b14 6666 560d 4989 3946 2ce4 18a4); the staff is drawn
    # with the same browns and is told apart by shape (thin)
    FIGHTER: {"suit": [1, 2, 3, 4], "skin": [6, 12, 7, 13, 8, 14, 9], "white": [5], "belt": [11],
              "tip": [10], "wood": [], "black": [15]},
    # the thug's shirt is always the tunic and his trousers the legs (his frames lie down)
    THUG: {"suit": [1, 2, 3, 4, 6, 7, 8, 9], "torso": [1, 2, 3, 4], "legs": [6, 7, 8, 9],
           "skin": [11, 12, 13, 14], "white": [5], "belt": [], "hair": [10], "tip": [], "wood": [],
           "black": [15]},
}
# the thug's crouch: his trousers are the shirt's colour (split at the waist like the fighter's),
# 6-9 are his boots and chains (the chains go; Robin gets a staff, CROUCH_STAFF)
CROUCH = {"suit": [1, 2, 3, 4], "skin": [11, 12, 13, 14], "white": [5], "belt": [], "hair": [10],
          "tip": [], "wood": [], "black": [15], "chain": [6, 7, 8, 9]}

# player animation id -> source frames, resampled onto that animation's slots
F, T = FIGHTER, THUG
ANIM_SOURCES = {
    0: (F, [69, 70, 71, 74, 75]),               # B: thrust (the fighter's own sequence)
    1: (F, [110, 111, 112, 113, 116, 117, 118]),  # B again: upswing
    2: (F, [59, 60, 61, 62, 63, 64]),           # big swing
    3: (F, list(range(93, 103))),               # hero attack: spin
    4: (F, [83, 84, 85, 86, 87, 88, 89, 90]),   # hero attack: overhead into a ground sweep
    5: (F, [42, 51, 52, 53, 54, 57, 58]),       # wind-up and thrust
    6: (F, [86, 87, 88, 89]),                   # down+B: low sweep
    7: (F, list(range(97, 104))),               # spin, second half
    8: (F, [23]), 9: (F, [0]), 10: (F, [23]), 11: (F, [23, 24]),   # idles: upright
    12: (F, [25, 26, 27, 28, 29, 30, 31, 32]),  # run (loops)
    13: (F, [32, 33, 39, 40]), 14: (F, [33]),
    15: (F, [34, 35, 36, 37, 38, 39, 40]),      # jump
    16: (F, [34, 35, 36, 37, 38]),
    17: (F, [36, 37, 57, 58, 58]),              # jump kick -> staff thrust in the air
    19: (T, [39, 39]), 20: (T, [39]), 21: (T, [39]),   # crouch: the thug's (no fighter crouch)
    22: (F, [23]),                              # stance (after a crouch, between moves)
    23: (T, [78, 79]), 24: (T, [79, 80]),       # hurt
    25: (T, [78, 79, 80, 81, 82, 83]),          # knocked down
    26: (T, [83, 84, 85, 86, 87, 88]),          # lying, getting up
    27: (F, [71, 72]), 28: (F, [39, 40]), 29: (F, [39, 40]), 30: (F, [81, 82]),
    31: (F, [71, 72]), 32: (F, [39, 40]),       # Birdarang throws
    34: (F, [0]), 35: (F, [23]), 36: (F, [81, 82]), 37: (F, [34, 35]),
}
EXTRA_SLOTS = {35: (F, 58), 36: (F, 38), 37: (F, 39)}  # slots the table reaches only via ranges/loops
HERO_ANIMS = {3, 4, 7}
FPS_ATTACK = 13          # the staff fighter: 4-5 ticks a frame
FPS_HERO = 20
ATTACK_ANIMS = {0, 1, 2, 5, 6, 17}

MELEE_RANGE_CMP = 0x0DADD5   # FUN_0dada3: cmp r3, 0x28 (6-bit immediate)
MELEE_RANGE = 60

CROUCH_STAFF = ((-3, -5), (25, -16))   # through the thug's right hand at (11, -11)

BIRD_SPEED_INSN = 0x0657AF   # FUN_0657a7: r3 = 03, the Batarang's speed (phase step = speed/60)
BIRD_SPEED = 5
BIRD_RANGE = 0x5998          # per-direction target offsets; straight ahead 0xB4 = 180 px
BIRD_RANGE_SCALE = 4 / 3
BIRD_TILES = range(0x522, 0x530)  # spin frames, palette bank 13 (shared with the HUD)
BIRD_COLOURS = {7: 11, 8: 12, 9: 14, 10: 15, 3: 13, 4: 14, 5: 15}


def compose(rom, rec):
    """{(x, y): palette index} of a frame, pieces drawn in order (y down)"""
    img = {}
    for x, y, t in rec["pieces"]:
        px = sprites.tile_pixels(rom, t)
        for ty in range(16):
            for tx in range(16):
                if px[ty][tx]:
                    img[(x - 8 + tx, y - 8 + ty)] = px[ty][tx]
    return img


def component(pixels, start):
    seen, todo = {start}, deque([start])
    while todo:
        x, y = todo.popleft()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                q = (x + dx, y + dy)
                if q in pixels and q not in seen:
                    seen.add(q)
                    todo.append(q)
    return seen


def ramp(v, src, dst):
    """map a source ramp index to the destination ramp of the same relative brightness"""
    i = src.index(v) * len(dst) // len(src)
    return dst[min(i, len(dst) - 1)]


def robinize(img, cls):
    suit, skin = cls["suit"], cls["skin"]
    body = [p for p, v in img.items() if v in suit or v in skin]
    if not body:
        return dict(img)
    top, feet = min(y for _, y in body), max(y for _, y in body)
    height = feet - top + 1
    def thin(p):  # the staff: pixels with few filled neighbours (a 1-2 px line)
        x, y = p
        return sum((x + dx, y + dy) in img for dx in range(-2, 3) for dy in range(-2, 3)) <= 12

    staff = {p for p, v in img.items() if cls is CLASSES[FIGHTER] and (v in suit or v in skin)
             and thin(p)}
    skinpx = {p for p, v in img.items() if v in skin and p not in staff}
    # the head: skin above the collar (the middle of the suit's top rows); a raised hand or the
    # staff can be higher, so it isn't simply the topmost skin
    head = set()
    # the body: the biggest patch of suit (the staff's coloured tips are suit colours too)
    suitall = {p for p, v in img.items() if v in suit and p not in staff}
    suitpx, rest = set(), set(suitall)
    while rest:
        part = component(rest, next(iter(rest)))
        rest -= part
        suitpx = max(suitpx, part, key=len)
    if suitpx:
        s_top = min(y for _, y in suitpx)
        s_xc = sum(x for x, y in suitpx if y <= s_top + 3) / len([1 for _, y in suitpx if y <= s_top + 3])
    if skinpx and suitpx:
        collar = [x for x, y in suitpx if y <= s_top + 2]
        xc = sum(collar) / len(collar)
        cand = {p for p in skinpx if p[1] <= s_top + 2 and abs(p[0] - xc) <= 6}
        if cand:
            h0 = min(cand, key=lambda p: (p[1], abs(p[0] - xc)))
            # at most 8 rows and 11 px wide: an arm across the chest touches it but isn't it
            head = {p for p in component(skinpx, h0)
                    if p[1] <= min(s_top + 2, h0[1] + 7) and abs(p[0] - xc) <= 5}
    if True:  # the first row of skin at least 3 px wide (a staff isn't): the top of the head;
              # it replaces the collar guess, which can land on a shoulder or the staff
        allskin = {p for p, v in img.items() if v in skin}
        wide = [p for p in allskin if all((p[0] + i, p[1]) in allskin for i in range(3))
                and (not suitpx or "torso" in cls or (s_top - 20 <= p[1] <= s_top + 4 and abs(p[0] + 1 - s_xc) <= 10))]
        if wide:
            h0 = min(wide, key=lambda p: (p[1], p[0]))
            if h0[1] - top < 0.3 * height:
                head = {p for p in component(allskin, h0)
                        if h0[1] <= p[1] <= h0[1] + 7 and abs(p[0] - h0[0] - 1) <= 4}
                staff -= head
    neck = max(y for _, y in head) if head else top + height // 6
    # the belt counts only where a belt can be (a staff highlight can be the same colour)
    belt = [y for (x, y), v in img.items() if v in cls["belt"]
            and neck + 0.2 * (feet - neck) <= y <= neck + 0.65 * (feet - neck)]
    waist = max(belt) if len(belt) >= 2 else neck + int(0.42 * (feet - neck))

    # the thug's boots/shins share the chain's colours: they are the part that reaches the feet
    chain = {p for p, v in img.items() if v in cls.get("chain", ())}
    shins = set()
    for q in chain:
        if q[1] >= feet - 1 and q not in shins:
            shins |= component(chain, q)

    out = {}
    for p, v in img.items():
        x, y = p
        if v in cls.get("chain", ()):
            if p in shins:
                out[p] = ramp(v, cls["chain"], [GRN, GRN, GRN_D, GRN_D])
            continue                                   # the chain goes
        if p in staff:
            out[p] = YEL_L if v in suit[:1] else WOOD if v in suit or v in skin[:2] else BRN_D
        elif v in cls.get("torso", ()):
            out[p] = ramp(v, cls["torso"], [RED_L, RED, RED_D])
        elif v in cls.get("legs", ()):
            out[p] = ramp(v, cls["legs"], [GRN_L, GRN, GRN_D])
        elif v in suit:
            out[p] = ramp(v, suit, [RED_L, RED, RED_D] if y <= waist else [GRN_L, GRN, GRN_D])
        elif v in skin:
            # bare shoulders/arms: tunic above the waist, green gloves below
            out[p] = ramp(v, skin, [SKIN_L, SKIN, SKIN_D] if p in head else
                          [RED_L, RED, RED_D] if y <= waist else [GRN_L, GRN, GRN_D])
        elif v in cls.get("hair", ()):
            out[p] = BLACK
        elif v in cls["white"]:
            out[p] = WHITE
        elif v in cls["belt"] or v in cls["tip"]:
            out[p] = YEL
        elif v in cls["wood"]:
            out[p] = ramp(v, cls["wood"], [WOOD, WOOD, BRN_D])
        else:
            out[p] = BLACK

    if head:
        x0, x1 = min(x for x, _ in head), max(x for x, _ in head)
        y0, y1 = min(y for _, y in head), max(y for _, y in head)
        # Robin's head, drawn once (HEAD) and put where the source head was: chin on its chin,
        # facing forward. The source head (and the thug's hair) goes.
        for q in head:
            out.pop(q, None)
        for q, v in img.items():   # the face's edges the head region left out (nose, jaw)
            if v in skin and q not in staff and x0 - 2 <= q[0] <= x1 + 3 and y0 - 3 <= q[1] <= y1:
                out.pop(q, None)
        for q, v in img.items():
            if v in cls.get("hair", ()) and q in out and abs(q[0] - (x0 + x1) / 2) <= 7 \
                    and y0 - 6 <= q[1] <= y1:
                del out[q]
        # upright: on top of the shoulders, even where the source hunches forward (the run) and
        # its head hangs in front of the chest
        if suitpx:
            t_top, xc = s_top, round(s_xc)
        else:
            t_top, xc = y1, (x0 + x1 + 1) // 2
        cx = (x0 + x1 + 1) // 2 + HEAD_SHIFT
        if "torso" not in cls:   # not the thug lying down
            cx = min(max(cx, xc - 3), xc + 2)
        hx, hy = cx - len(HEAD[0]) // 2, y1 - len(HEAD) + 1
        for r, row in enumerate(HEAD):
            for k, ch in enumerate(row):
                if ch != ".":
                    out[(hx + k, hy + r)] = HEAD_COLOURS[ch]

        # a narrow cape close to the back, from the shoulders to the upper thigh
        shoulder, bottom = y1 + 1, waist + (feet - waist) // 3
        torso = {p for p, v in img.items() if (v in suit or v in skin) and p not in staff}  # the back silhouette
        if height >= 30 and bottom > shoulder:
            for y in range(shoulder, bottom + 1):
                row = [x for x, yy in torso if yy == y]
                if not row:
                    continue
                xl = min(row)
                t = (y - shoulder) / (bottom - shoulder)
                w = 1 + (t > 0.35) + (t > 0.75)
                for i in range(1, w + 1):
                    if (xl - i, y) not in out:
                        out[(xl - i, y)] = BRN_D if i == w else YEL
    return out


def draw_staff(img, a, b):
    """a staff from a to b behind the body: wood with a dark underside, yellow tips"""
    (xa, ya), (xb, yb) = a, b
    n = max(abs(xb - xa), abs(yb - ya))
    for i in range(n + 1):
        x, y = xa + (xb - xa) * i // n, ya + (yb - ya) * i // n
        tip = i < 2 or i > n - 2
        for q, v in (((x, y), YEL_L if tip else WOOD), ((x, y + 1), YEL if tip else BRN_D)):
            img.setdefault(q, v)


def cut(img, src_pieces):
    """16x16 pieces covering img: the source's piece positions, then extra ones for the rest"""
    pieces, covered = [], set()
    for x, y, _ in src_pieces:
        tile = [[img.get((x - 8 + tx, y - 8 + ty), 0) for tx in range(16)] for ty in range(16)]
        if any(v for r in tile for v in r):
            pieces.append((x, y, tile))
        covered |= {(x - 8 + tx, y - 8 + ty) for tx in range(16) for ty in range(16)}
    left = {p for p in img if p not in covered}
    while left:
        px, py = min(left, key=lambda p: (p[1], p[0]))
        best = max(range(-15, 1), key=lambda o: sum((px + o + tx, py + ty) in left
                                                    for tx in range(16) for ty in range(16)))
        x0 = px + best
        tile = [[img.get((x0 + tx, py + ty), 0) for tx in range(16)] for ty in range(16)]
        pieces.append((x0 + 8, py + 8, tile))
        left -= {(x0 + tx, py + ty) for tx in range(16) for ty in range(16)}
    return pieces


def resample(seq, n):
    return [seq[i * len(seq) // n] for i in range(n)]


def main():
    rom = sprites.load_rom(sys.argv[1])
    anims = [rom[ANIMS + 4 * i:ANIMS + 4 * i + 4] for i in range(43)]

    # slot -> (source list, source frame, animation id)
    slots = {}
    for a, (lst, seq) in ANIM_SOURCES.items():
        first, last = anims[a][0] - 1, anims[a][1] - 1
        for s, f in zip(range(first, last + 1), resample(seq, last - first + 1)):
            slots.setdefault(s, (lst, f, a))
    for s, (lst, f) in EXTRA_SLOTS.items():
        slots.setdefault(s, (lst, f, None))
    for s in range(PLAYER_SLOTS):
        slots.setdefault(s, (FIGHTER, 23, None))

    # free tiles: Batman's, used by frames 0-138 only
    bat = sprites.frames(rom)
    low = {t for f in TILE_AREA_FRAMES for _, _, t in bat[f]}
    high = {t for f in range(PLAYER_SLOTS, sprites.NFRAMES) for _, _, t in bat[f]}
    free = sorted(low - high)

    tiles, built = {}, {}   # tile pixels -> tile number; (lst, f, hero, slot boxes) -> record words
    records = []
    for s in range(PLAYER_SLOTS):
        lst, f, a = slots[s]
        src = sprites.record(rom, lst, f)
        mine = sprites.record(rom, sprites.FRAME_LIST, s)
        attack = [b for b in src["boxes"] if b[4] not in (1, 2)]
        tail = src["tail"]
        if not attack:
            attack = [b for b in mine["boxes"] if b[4] not in (1, 2)]
            if attack:
                tail = mine["tail"]
        if a in HERO_ANIMS:
            attack = [b[:4] + (0x14,) for b in attack]
        key = (lst, f, tuple(attack), tail, mine["flags"], mine["word2"])
        if key not in built:
            img = robinize(compose(rom, src), CROUCH if (lst, f) == (THUG, 39) else CLASSES[lst])
            if (lst, f) == (THUG, 39):
                draw_staff(img, *CROUCH_STAFF)
            pieces = []
            for x, y, px in cut(img, src["pieces"]):
                k = tuple(v for r in px for v in r)
                if k not in tiles:
                    if len(tiles) == len(free):
                        sys.exit(f"out of tiles at slot {s} ({len(free)} available)")
                    tiles[k] = free[len(tiles)]
                pieces.append((x, y, tiles[k]))
            body = [b for b in src["boxes"] if b[4] in (1, 2)]
            rec = {"flags": mine["flags"], "word2": mine["word2"], "boxes": body + attack,
                   "tail": tail, "pieces": pieces}
            built[key] = sprites.encode_record(rec)
        records.append(built[key])

    # pack the records where Batman's 0-138 were
    start = sprites.BANK + rom[sprites.FRAME_LIST]
    end = sprites.BANK + rom[sprites.FRAME_LIST + PLAYER_SLOTS]
    data, where = [], {}
    offsets = []
    for w in records:
        k = tuple(w)
        if k not in where:
            where[k] = start + len(data)
            data += w
        offsets.append(where[k] - sprites.BANK)
    if start + len(data) > end:
        sys.exit(f"records need {len(data)} words, {end - start} available")

    def words(addr, ws, note=""):
        for k in range(0, len(ws), 16):
            print(f"words {addr + k:06x} " + " ".join(f"{w & 0xFFFF:04x}" for w in ws[k:k + 16])
                  + (f"   # {note}" if note and k == 0 else ""))

    print("# generated by mods/robin/generate.py (do not edit): Robin from the staff fighter")
    print("name robin")
    print(f"rom  {sprites.SHA}")
    words(sprites.PALETTE + 1, [PALETTE[i] for i in range(1, 16)], "Robin's palette")
    words(start, data, f"{len(where)} frame records")
    words(sprites.FRAME_LIST, offsets, "frame list 0-138")
    for px, t in tiles.items():
        rows = [list(px[i * 16:i * 16 + 16]) for i in range(16)]
        words(t * 64, sprites.pixels_to_words(rows))
    for a in range(43):
        fps = FPS_HERO if a in HERO_ANIMS else FPS_ATTACK if a in ATTACK_ANIMS else anims[a][3]
        if fps != anims[a][3]:
            words(ANIMS + 4 * a + 3, [fps], f"animation {a} speed")
    assert rom[MELEE_RANGE_CMP] == 0x4668
    words(MELEE_RANGE_CMP, [0x4640 | MELEE_RANGE], "melee range 40 -> 60 px")
    assert rom[BIRD_SPEED_INSN] == 0x9643
    words(BIRD_SPEED_INSN, [0x9640 | BIRD_SPEED], "Birdarang speed 3 -> 5")
    words(BIRD_RANGE, [round(sprites.s16(rom[BIRD_RANGE + d]) * BIRD_RANGE_SCALE)
                       for d in range(8)], "Birdarang range")
    for t in BIRD_TILES:
        px = [[BIRD_COLOURS.get(v, v) for v in row] for row in sprites.tile_pixels(rom, t)]
        words(t * 64, sprites.pixels_to_words(px))
    print(f"# {len(where)} records ({len(data)} of {end - start} words), "
          f"{len(tiles)} of {len(free)} tiles", file=sys.stderr)


if __name__ == "__main__":
    main()
