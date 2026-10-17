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

- `mods/robin` (`generate.py`) gives Robin's look. The rules are applied per assembled frame, and
  shared tiles take the majority result:
  - cowl pixels next to the face become black hair (the ears become spikes);
  - suit pixels below the belt become green (where no belt shows, below 45% of the body height);
  - the barely used colours 1 and 5 become the greens.

  350 tiles change. MAME running the baked ROM matches the native run over 6100 vblanks of
  `session2` (3 snapshots identical).
- Loader bug found on the way: `words` lines in `mod.txt` only applied their first 7 values (the
  tokenizer kept 8 tokens). They now take any number.
- `sprites.py export` writes the tiles as an indexed PNG in the character's palette.
  `sprites.py import` turns an edited sheet back into `words` lines, covering only the changed
  tiles.

## Melee (found for Robin's staff)

- RAM 0x0184 is the player's current frame index.
- **The box format, confirmed.** In a hit frame the game reads the boxes as x0, x1, y0, y1, type,
  then the effect words. The types are:
  - 1 and 2: body;
  - 3: punch;
  - 4: kick;
  - 20 (0x14): hero attack.

  Attack frames end with (effect id, spark x, spark y).
- **Melee or Batarang.** B attacks in melee only if `FUN_0dada3` finds an enemy within 40 px across
  (`cmp r3, 0x28` at 0x0DADD5, a 6-bit immediate) and 24 px in depth. Otherwise B throws the
  Batarang.

  Measured with the first enemy pinned at distance D (`poke 0275/0288 always`): Batman punches up
  to D = 38 and throws from 42.
- **Damage.** `FUN_0070d9` applies a pending hit (object +0x1B: type in the low nibble) to the
  target. It is 2 for a melee hit, 1 for the Batarang, and more for some hero attacks.
- **Enemy slots** are 76 words starting at 0x026B: +0x0A X (copy at +0x1D), +0x0B Y, +0x15
  health (4 for the first thugs).
- **`mods/robin`'s staff:**
  - the range check goes from 40 to 60 px, and the forward attack boxes and sparks move 20 px
    further out. Robin then hits for 2 out to D = 62;
  - `plugin.c` draws the staff as 2 extra sprites, because the shared sprite bank fills its 64K
    window and the frames can't grow;
  - the staff tiles are in the ROM's upper half, so the staff only shows in our runtime.

## Open

- The bank header's other tables: animations, which frame sequences and timings make up each move.
- What the box words mean, and the code that tests them (melee reach, hurt box).
- The Batarang: its object, its path and its sprite frames.
