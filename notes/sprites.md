# Player sprites

Found by watching the ROM reads (`batman-rt --watch`) in a frame that draws Batman, then matching
them to the sprite list of a `--vdump`. `tools/sprites.py` implements this; `sprites.py frames`
draws all 183 frames correctly assembled.

## Format

- **Bank.** Batman's sprite bank starts at ROM 0x051C40. Offsets inside it are relative to that.
  The bank's first words are a table of offsets to sub-tables; only some are understood so far.
- **Frames.** 0x054B4B holds 183 frame offsets. A frame is:
  - count, flags, ?, k;
  - k words of boxes;
  - 3 words;
  - count pieces of (dx, dy, tile index).

  k is 0, 10 or 15. The box words are signed offsets; they are probably the body box and the attack
  boxes, which the staff will need (open).
- **Pieces.** The first piece is placed relative to the frame origin, and each later one relative
  to the piece before it (y down). The tile number is index + 0x400: a 16×16 4bpp tile at ROM word
  tile × 64.
- **Tiles.** The frames use 551 tiles, 0x0607–0x0836. The Batarang and effects use other tiles
  with the same palette (0x0536–0x053D, 0x05B0–0x05C6 seen on screen).
- **Palette.** Sprite bank 14, 16 words at 0x051870:
  - 0 transparent;
  - 1–4 the cape, which includes the cowl (blues);
  - 5–9 the suit (greys);
  - 10–14 the belt and skin;
  - 15 black.

  The game loads it at level start and at each respawn (seen via `--watch`). The lives heads in
  the HUD use bank 13 and other tiles.

## Mods

- `mods/robin` recolours the palette: the cape becomes yellow and the suit red. MAME running the
  baked ROM matches the native run over 6100 vblanks of `session2` (3 snapshots identical).
- `sprites.py export` writes the tiles as an indexed PNG in the character's palette.
  `sprites.py import` turns an edited sheet back into `words` lines, covering only the changed
  tiles.

## Open

- The bank header's other tables: animations, which frame sequences and timings make up each move.
- What the box words mean, and the code that tests them (melee reach, hurt box).
- The Batarang: its object, its path and its sprite frames.
