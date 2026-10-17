# RAM map

Word addresses. Found with `tools/ramsearch.py` over the RAM dumps (`batman-rt --dump`) of two
recordings, both of which match MAME on every frame:
- `tests/inputs/session1.txt`: 68,520 vblanks;
- `tests/inputs/session2.txt`: 11,760 vblanks, Street Party played to Game Over. Each entry says how it
was confirmed:
- **poke**: a mod pinned the value, and the screen changed as predicted;
- **math**: it predicts register values or positions;
- **trace**: its history fits the recorded play.

## Confirmed

| Addr | Meaning | Values | How |
|---|---|---|---|
| 0x015A | level id | 0x0C Street Party (set at vblank 3775), 0x0D Fast Lane (48709); 0xF00D before the first level | trace |
| 0x01E9 | player health (brawler levels) | 0x20 = full; −2 per hit, +4 per pickup | poke: 6 → bar shows a sliver; `mods/example-cheat` |
| 0x0244 | SPECIAL meter (brawler); 0x0A in Fast Lane, probably BOOST there | 0–0x20 | poke: 0x20 → meter full |
| 0x0191 | player X in level pixels (copies at 0x019C, 0x01C1) | 0x0194 at 4450, 0x0A9F at 11050 | math: X − camera X = Batman's sprite position |
| 0x0192 | player Y? (0x83 jumping at 4450, 0xC8 on the ground at 11050) | | trace, unconfirmed |
| 0x208E | camera X in level pixels (copy at 0x2603) | | math: written to page 2's X scroll (0x2816) the next frame, in 100% of frames; page 1 gets half (parallax) |
| 0x208F | camera Y (copy at 0x2604) | 5 at 11050 = page 2's Y scroll | math |
| 0x2099 | lives (the heads under the health bar) | 4 at the start; −1 per death (session2: 4→0 at vblanks 5634, 7060, 8474, 9895), Game Over after the death at 0 | poke: 1 → one head; 9 → nine heads and no Game Over |
| 0x0150–0x015A | brawler section loop, pointed to by 0x270B | [0] length in tile columns, [1] start column, [7] map column, [8] = 0x0158 section number (0..0x0B in Street Party) | code: FUN_00e74e (notes/level-format.md) |
| 0x01E8 | +0x0C at each respawn (f019, f025, f031...) | | trace, meaning unknown |
| 0x25E9, 0x2603 | layer structs for page 1 and page 2 (map pointer, width, tile base, camera limits) | | code: FUN_00bfb5 (notes/level-format.md) |
| 0x0809 | vblank counter, incremented by the IRQ handler | | code (notes/game-structure.md) |
| 0x088E–0x108D, 0x108E–0x148D | page 1 tilemap and tile attributes | | PPU registers (notes/graphics.md) |
| 0x148E–0x1C8D, 0x1C8E–0x208D | page 2 tilemap and tile attributes | | PPU registers |

## Candidates

- 0x015D: follows 0x0158, the section number.
- 0x0180: moves with the player. It's 0x2C less than player X at 11050, maybe a hitbox edge.
- 0x0176: climbs steadily while walking (0x0327 → 0x04CB over 4400–4700). Unknown.
- 0x0170–0x01C7: looks like the player object (positions, state words 0x8000/0x8016...).

## Fill patterns

The game fills unused RAM with 0xF00D (789 words stayed 0xF00D through Street Party) and
0xDEAD (79 words). This shows which areas a level leaves unused.

## How to search

```
F=<workdir>/ours/frames.bin     # from tests/check.sh, or batman-rt --dump
tools/ramsearch.py split F 5000 47000 50200 61000     # constant per level, different between
tools/ramsearch.py corr  F 4000 20000 tests/inputs/session1.txt   # follows Right/Left
tools/ramsearch.py stats F 3800 48000 --max-distinct 40 --min-changes 3
tools/ramsearch.py trace F 3700 48700 1e9             # history of one word
```

Then confirm with a `poke ... always` mod and a snapshot.
