# Characters in the sprite bank

Found for the versus mode (`notes/versus.md`). `tools/sprites.py chars` lists every entry and draws a
few frames of each. `tools/sprites.py --char HEADER frames <rom> out.png` draws all the frames of one
entry in its own palette. Word addresses.

## The bank's header

- **The bank.** It starts at 0x051C40.
  - Word 0 is the number of entries (0x5F).
  - Then comes one offset per entry to a character header. Several entries share a header, so
    there are 57 distinct ones.
- **A character header:**
  - word 0: the frame count;
  - words 1–7: palette ids, where 0xFFFF means none. The first is the default; thugs list their
    colour variants;
  - words 8–11: a default box;
  - from word 0x0C: the frame list, one offset per frame.

  Batman's header 0x2EFF gives frame list 0x054B4B and palette id 3, which matches
  `notes/sprites.md`.
- **Palettes.** Palette id N is the 16 words at **0x051840 + 16·N**.
  - Batman, id 3, is at 0x051870.
  - The staff fighter's ids 0x1B–0x1F are at 0x0519F0–0x051A30. 0x051A00, the yellow one in
    `mods/robin`, is id 0x1C.
  - Colour 0 has bit 15 set (transparent).
- **One sprite bank.** All 13 reads of the bank base in the code (0x00B330 … 0x00BCC8) load
  05:1C40, so there is only this bank.
- **Frames.** Every character's frames are drawn facing right. Box types:
  - 1, 2: body;
  - 6: a boss's body;
  - 3, 4, 0x14: attacks;
  - 5: Firefly's flame column.
- **Origin.** The frame origin is the feet.

## Who is who

Identified by eye from the frame sheets in each one's own palette.

| header | frames | palette ids | what |
|---|---|---|---|
| 0x2EFF | 183 | 3 | Batman |
| 0xB682 | 75 | 0x25 | **the Joker**: purple suit, green hair. The mallet swing 23–29 has attack boxes, the gun 38–47, a jump kick 4–6, a crouched walk 49–60, laughing 64–72. `notes/sprites.md` had listed this list (0x05D2CE) as Harley Quinn. |
| 0xBFB5 | 35 | 0x26 | **Bane**, the red Venom form, standing on a hanging window-washer platform that is drawn into every frame (tiles 0x10E2–0x10EC). Idle 0–20, electrocuted 21–30 (white flashes on 22, 23, 25, 27), staggering 31–34. No walk or attack frames. Without the platform he is about 67×80 px, feet at y 0. |
| 0x57EA | 72 | 0x0D | **Firefly**: hovering 0–9, flamethrower 10–13, standing 14–25, the flame column 27–39, flying 40–51, a kick 52–55, crouch 56–61. |
| 0x489F | 30 | 0x0A | **Mr. Freeze**: standing, then his freeze breath (21–29). |
| 0x4D64 | 97 | 0x0B | **the Penguin** (umbrella, chain) |
| 0x8699 | 206 | 0x1B–0x1F | the staff fighter (`mods/robin`'s source): stance 17–24, walk 9–16, flips 32–40, thrusts 69–75, long swing 57–62, sweep 84–88, staff spin 93–107, rising thrust 109–113, hurt 138–139. Frames 140 on are another enemy (a chain, no staff). |
| 0xD520 | 217 | 0x2E–0x33, 0x19 | thug with fall, lie and get-up frames (`notes/sprites.md`'s 0x05F16C) |
| 0x7426 | 153 | 0x13– | a slim kickboxer: punches 57–59, high kick 71–75, flying kick 81–84, sweep 91–94, low kick 106–108, flips 33–40, throws 116–133, hurt 134–141 (Catwoman's stand-in) |
| 0x5F49, 0xA355 | 153, 159 | 0x0E–, 0x20– | thugs (a pipe, a club) |
| 0x0060 … 0x2BD6 | | 0, 1 | HUD and effects (the bat icon, "GO", digits, sparks) |
| 0x2ED4, 0x4640 … 0x487A | 1 each | | vehicles and props of the driving levels |
| 0xF14E … 0xFA67 | | | small objects and effects |

## Open

- Bane has no walking or punching frames in this list. The boss may be drawn with other pieces
  in *Bat Meets Monster*, which no recorded session reaches yet.
- The default box (header words 8–11) isn't decoded.
- Which header each level's enemies use, and the enemies' own animation tables (Batman's are at
  ROM 0x5744, `notes/sprites.md`).
