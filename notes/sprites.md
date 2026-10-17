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
  - (v1 drew the staff with a plugin; v2 uses the staff fighter's own staff, see below.)

## Batarang (found for Robin's Birdarang)

- **Object.** It is an object in the same 76-word slots (slot 2 in the test: X at 0x030D). Its
  sprite is one 16×16 tile cycling through 14 spin frames, 0x0522–0x052F, in palette bank 13
  (shared with the HUD).
- **Motion.** `FUN_065b64` throws it:
  - the target is the thrower's X plus a per-direction offset (8 directions, table at 0x5998;
    0xB4 = 180 px straight ahead) plus up to ±16 px at random;
  - other per-direction tables sit at 0x5988–0x59C7.

  Each leg runs a 16.16 phase from 0 to 1, eased (`FUN_0dba85` adds the step each frame). The step
  is SPEED/60, where SPEED is the immediate in `r3 = 03` at 0x0657AF (`FUN_0657a7`). That gives
  20 frames a leg.
- **Return.** `FUN_065aa1` turns it back on a hit, or at the end of the leg, towards the
  thrower.
- **Robin's Birdarang** (`mods/robin`): SPEED 5 (12 frames a leg), range ×4/3 (240 px), and the
  spin frames recoloured red/yellow. Thrown at an enemy pinned 150 px away, it hits in 6 frames;
  the Batarang tops out at about 129 px there and misses.

## The whole sprite bank, and animations (found for Robin v2)

- **Frame lists.** The bank at 0x051C40 holds about 30 frame lists, one or more per character.
  Each is a run of offsets whose records end exactly where the next begins. The ones used so far:

  | List | Character |
  |---|---|
  | 0x054B4B | Batman, 183 frames |
  | 0x05A2E5 | the yellow staff fighter (Street Party, 2nd half), 130 frames |
  | 0x05A36F | the bomb thrower |
  | 0x05F16C | a thug with fall, lie and get-up frames (78–88) |
  | 0x05D2CE | Harley Quinn (mallet) |

  Palettes: staff fighter at 0x051A00, thug at 0x051B50.
- **Batman's frames by use.**
  - 0–138 come from the animation table.
  - 144–150 and 161–165 are a separate cape, and 151–182 a smaller capeless body.
  - 139–182 never appear in session1.
- **Player animations, ROM 0x5744.** 4 words per animation: first frame, last frame, loop-to
  frame (1-based), fps.
  - `FUN_0daf22(id)` starts one. RAM 0x0184/0x0185 is the 16.16 frame counter, stepped by fps/60
    a tick (0x0186/0x0187).
  - There are 44 animations: 0–7 the B combos and hero attacks, 8–14 idles and the run, 15–18
    and 38–41 jumps, 19–22 fight stances, 23–26 hurt, knockdown and getting up, 27–37 Batarang
    throws and others.
- **Staff fighter timing**, measured by matching on-screen sprites to its frames in session1 (RAM
  dump): each frame is held 4–5 ticks. The sequences are thrust 69, 70, 71 (74, 75), wind-up 42
  into 51–54, and upswing 110–113 into 116–118.
- **`mods/robin` v2** (`generate.py`) rebuilds the player's frames 0–138 from staff-fighter frames
  (thug frames for the knockdown):
  - each frame is repainted by rule (head, hair, mask, tunic/legs split, staff by shape, cape)
    and re-cut into pieces;
  - the new tiles take the place of Batman's 443 tiles that only frames 0–138 use (it uses about
    415);
  - the frame records go in Batman's record space (about 4100 of 4900 words);
  - the animation table keeps its slots, with attacks at 13 fps;
  - the melee range is 60.

  Everything is data. MAME running the baked ROM matches the native run over 6100 vblanks of
  session2 (3 snapshots).

## Open

- The bank header's other tables: animations, which frame sequences and timings make up each move.
- What the box words mean, and the code that tests them (melee reach, hurt box).
- The Batarang: its object, its path and its sprite frames.
