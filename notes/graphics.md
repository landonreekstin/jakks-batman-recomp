# Graphics

Decoded with `tools/gfx.py` from video-state dumps (`batman-rt --vdump dir --snap-frames N`)
and the ROM. **Check:** `gfx.py screen` rebuilds the visible screen from its own decoding of
pages and sprites (priorities and blending included). It is pixel-identical to the runtime's
snapshot, and so to MAME, at four vblanks of `session1`:
- 1500: title menu
- 4450 and 11050: Street Party
- 50150: Fast Lane

## Hardware formats as this game uses them

- **Tiles**: 4bpp, 16 colours from one of 16 palette banks. Each word is byte-swapped, then read
  as nibbles from the top. An 8×8 tile is 16 words and a 16×16 tile 64 words.
- **Graphics base**: all three graphics segment registers (0x2820, 0x2821 for the pages, 0x2822
  for sprites) are 0. A tile number is therefore an absolute ROM address:
  - 8×8 tiles: word = tile × 16;
  - 16×16 sprite tiles: word = tile × 64.

  Tile 0 means "empty".
- **Palette**: 256 RGB555 entries at 0x2B00 (video RAM). Bit 15 set means transparent. The
  game rewrites the palette per scene.
- **Pages** (both seen identically configured in the brawler and driving levels):

  | | page 1 | page 2 |
  |---|---|---|
  | attr / ctrl | 0x0001 / 0x0108 | 0x1001 / 0x0108 |
  | tilemap (RAM) | 0x088E–0x108D | 0x148E–0x1C8D |
  | tile attributes (RAM) | 0x108E–0x148D | 0x1C8E–0x208D |
  | priority | 0 | 1 |

  - Each is a 64×32 grid of 8×8 tiles (512×256 pixels). The attribute byte per tile holds
    the palette bank (bits 0–3), flip x (4), flip y (5) and blend (6).
  - Scroll is registers 0x2810/0x2811 (page 1) and 0x2816/0x2817 (page 2).
  - No row scroll, linemap/bitmap or vertical compression has been seen.
- **Sprites**: 256 × 4 words at 0x2C00: tile, x, y, attr.
  - 0x2842 = 1: sprites on, with centred coordinates (screen x = 160 + x − w/2,
    y = 128 − y − h/2).
  - Characters are built from 16×16 4bpp pieces.
  - Blend is attr bit 14, at blend level 0x282A = 2 (0x18/0x20).

## What the game puts where (Street Party, vblank 11050)

- **Page 1**: parallax skyline. Tiles come from ROM words 0x080010–0x088B7F.
- **Page 2**: street foreground. Tiles come from 0x080000–0x09877F, mostly 0x096CF0–0x09877F.
  - The tilemap is a ring buffer: the visible 320 px window moves through the 512 px map, and
    the game writes new tile columns ahead of the camera. The level layout itself therefore
    lives elsewhere in ROM, in the game's own format: that's the level-format work.
- **Sprites**:
  - Batman is #14–19, tiles 0x066C–0x0671 (ROM 0x019B00–0x019C7F), palette bank E.
  - Enemies use palette bank B, tiles around 0x11B6 and 0x129A (ROM 0x046D80+, 0x04A680+).
  - The HUD is priority 3, palette bank D, #20–46. The health bar is a row of 16×16 tiles
    0x0592–0x05A0 (ROM 0x016480–0x01683F), and the tile at each slot varies with the value
    shown; the special meter is tiles 0x05B0–0x05C2.

## Tools

- `gfx.py layers <rom> <vdump> <dir>` writes:
  - `page1.png` and `page2.png`: full maps, with the visible window outlined;
  - `sprites.png`: each sprite on its own;
  - `palette.png`;
  - `report.txt`: registers, the sprite list with ROM addresses, and the tile regions in use.
- `gfx.py sheet <rom> <vdump> <word-addr> <count> <out.png> [bpp w h palbank]` views raw ROM
  tiles in a dump's palette, for browsing graphics banks.

## Open

- Where each palette comes from in ROM (the game copies them into 0x2B00).
- The compressed or raw form of the level layouts, and how columns get streamed into the ring.
