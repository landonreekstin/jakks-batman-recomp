# Co-op: what the game does, and how two players fit in

Word addresses. Found with `batman-rt --trace` (call trees built from a one-frame trace taken
after a `--state-load`), `--watch` (with `fn:ADDR`, which logs only accesses made inside calls of
one function) and RAM dumps from session1.

## The frame loop (brawler, Street Party)

`main` loops at 0x0DC114:

| call | what |
|---|---|
| 0x005CDE | wait for vblank |
| 0x0DC745 | copy the sprite list to sprite RAM (0x0DC5EF) |
| 0x00C15F(1), 0x00C15F(0) | scroll the two background layers |
| 0x0DA03D | |
| 0x0DD77F | **read the controller**: [0x080A] = port A & 0xFFF0 (held), [0x080B] = held & ~previous (pressed) |
| 0x00F743 | |
| 0x07E8A5 | **update every object** |
| 0x07E804 (every 16th frame), 0x0DD16D, 0x0DA172, 0x00B6EA, 0x0DC64D, 0x00EEAB, 0x00F155 ... | |

## Objects

- **The object list.** 0x07E8A5 walks a linked list from [[0x26FA] + 2].
  - Each node is 76 words.
  - Word 2 is the next node, and words 0–3 are list links.
  - Word 6 points to a far-pointer pair {segment, offset} in ROM, which is the update function.
  - The update is called with the node's address as its only argument. The call is made by
    pushing a return address, then `pop sr, pc` from that pair.
- **Street Party at vblank 4450:**

  | node | update | what |
  |---|---|---|
  | 0x0130 | 0x00E40A | |
  | 0x063C | 0x065913 | an enemy |
  | 0x017C | 0x008BF7 | **the player** |
  | 0x0688 | 0x0068BD | an enemy |
  | 0x00E4 | 0x0DA35B | |
  | 0x01C8 | 0x0DD3D6 | |
  | 0x0214 | 0x07FF39 | |
  | 0x0260 | 0x0DD2CB | |

## The player update, 0x008BF7

Writes measured over session1's Street Party (vblanks 3775–48600, `--watch w:0000-27ff,fn:008bf7`):

- **Its own node, 0x017C–0x01C7.**
  - Position at 0x019C: X as 16.16 (0x019C integer, 0x019D fraction), Y at 0x019E/0x019F.
  - 0x0191/0x0192 are copies made every frame by 0x007FF6. Enemies read these.
  - Animation is at 0x0184–0x018F.
- **The special meter**, 0x0244/0x0245.
- **Enemy nodes** (0x0260–0x07FB, 76-word slots): its attacks hit them.
- **The sprite buffer**, 0x20C3+ (4 words a sprite), with its pointer at 0x20A0. The player
  draws itself during its update.
- **Effect slots**, 0x2465–0x2543 (14 words each).
- **The camera targets** in the layer structs: 0x25EC (page 1, half speed) and 0x2606 (page 2),
  through 0x00C12F(layer, x).
  - These come from the **camera follow, 0x00C695(x, y, ...)**, which aims at x − 160 and eases.
  - The update calls it at 0x0090A1 with [0x019C], [0x019E]. It is also called from 0x0087B7
    and 0x008828.
- **Input:** [0x080A] (3425 writes, presses it consumes).
- **Scratch and stack:** 0x26E7–0x26FE, 0x2790+.

It doesn't write health (0x01E9) or lives (0x2099); enemies and the HUD objects do.

## How co-op works (recomp/src/coop.c)

- **Hooks.** A hook at the entry of 0x008BF7 points the call's return address at a trampoline
  (0x3FFF00, in the upper ROM copy, which the game never runs).
- **The second update.** When P1's update returns there:
  - P2's words go into RAM: 0x0180–0x01C7, 0x01E9, 0x0244/0x0245;
  - P2's buttons go into 0x080A/0x080B;
  - the update is called again, returning to 0x3FFF01;
  - there P1's words come back.
- **Camera.** The camera follow's entry is hooked. P1's call aims at the midpoint of the two
  players; P2's returns at once.
- **Leash.** After both updates, the players are kept at most 272 px apart. Whoever moved apart
  is held back, using X and its copies.
- **Spawn.** P2 starts as a copy of P1, 40 px behind, at the first player update of each level.
- **Save states.** All of the state is in `spg_t.coop`, so save states keep it.

## Open

- **Enemy targeting:** enemies read 0x0191/0x0192, which is P1. Their attack hit tests and
  damage only reach P1.
- **P2's own health, lives and HUD; death and respawn.**
- **Robin's frame list and palette for P2.**
- **Doors and scene changes.**
- **The Fast Lane driving engine.**
