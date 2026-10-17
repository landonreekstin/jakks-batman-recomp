# ROM usage map

Made with `batman-rt --romcov` (with `BATMAN_RENDER=1`, so the PPU fetches too) and summarised
by `tools/romcov.py`. Each ROM-space word is tagged with who read it: code fetch, CPU data read,
PPU (graphics) or SPU (sound), plus the first vblank it was read in.

Sessions so far:
- `tests/inputs/level1-start.txt` (5000 vblanks): boot, the title, the menus and the start of
  the first level.
- `tests/inputs/session1.txt` (68,520 vblanks, played by the user). Its milestones:

  | vblank | what happens |
  |---|---|
  | ~2250 | save to slot A |
  | ~3700 | "Street Party" (side-scrolling brawler) starts |
  | ~48,700 | "Fast Lane" (Batcycle driving level) loads |
  | ~62,100 | Level Completed |
  | ~68,500 | session ends on the "Hole in the Wall" briefing |

Everything below is a lower bound until more of the game has been played;
`tools/romcov.py a.bin b.bin ...` merges maps.

## Findings

- **Nothing at or above word 0x100000 was read**, in either session, across both gameplay
  engines (brawler and driving). All reads fall in segments 0x00–0x0F, the
  first 1 M words (2 MiB) of the 4 MiB file. The file's two halves are identical except words
  0x0000–0x3FFD, which the low copy can't show anyway (RAM/I/O shadow it). That fits a 2 MiB
  ROM chip dumped as 4 MiB.
  - **For mods:** if more coverage confirms this, words 0x100000–0x1FFFFF are free space
    inside the ROM file, so a baked ROM still runs in MAME. The upper 0x200000–0x3FFFFF space
    is free too, but only in our runtime.
  - The high copy of the shadowed block (0x100000–0x103FFD) was not read either.
- Words touched per segment in session1 (by most specific use; 68,520 vblanks):

  | seg | code | data | gfx | sound |
  |---|---|---|---|---|
  | 00 | 25992 | 2452 | 0 | 0 |
  | 01 | 0 | 0 | 34368 | 0 |
  | 02 | 0 | 0 | 16192 | 0 |
  | 03 | 0 | 0 | 20096 | 0 |
  | 04 | 0 | 0 | 18584 | 0 |
  | 05 | 0 | 16903 | 0 | 0 |
  | 06 | 1933 | 1852 | 22024 | 0 |
  | 07 | 3221 | 0 | 20176 | 0 |
  | 08 | 0 | 0 | 2944 | 0 |
  | 09 | 0 | 636 | 10576 | 0 |
  | 0A | 0 | 5622 | 0 | 0 |
  | 0C | 0 | 5837 | 0 | 0 |
  | 0D | 10132 | 285 | 0 | 8395 |
  | 0E | 0 | 12170 | 0 | 27096 |
  | 0F | 0 | 11139 | 0 | 0 |

- Words touched per segment in level1-start (5000 vblanks):

  | seg | code | data | gfx | sound |
  |---|---|---|---|---|
  | 00 | 10352 | 1409 | 0 | 0 |
  | 01 | 0 | 0 | 8640 | 0 |
  | 02 | 0 | 0 | 448 | 0 |
  | 05 | 0 | 855 | 0 | 0 |
  | 06 | 2353 | 16 | 19432 | 0 |
  | 07 | 2065 | 0 | 16560 | 0 |
  | 08 | 0 | 0 | 7328 | 0 |
  | 09 | 0 | 992 | 0 | 0 |
  | 0A | 0 | 2290 | 0 | 0 |
  | 0D | 6144 | 237 | 0 | 5959 |
  | 0E | 0 | 3568 | 0 | 19476 |
  | 0F | 0 | 3988 | 0 | 0 |

  Code is in segments 0x00, 0x06, 0x07 and 0x0D, which matches Ghidra's function list.
  Graphics are in 0x01–0x08, sound samples in 0x0D–0x0E, and tables in 0x05 and 0x09–0x0F.
- **Level start.** Going into the first level (the input script presses A on slot A around
  vblank 2500), there's a burst of first reads between vblanks 2512 and 2520:
  - code: 0x0650A0–0x065CAE, 0x00B3DE, 0x07ECBD
  - data: 0x09EC10, 0x0AA238–0x0AD1BF
  - graphics: 0x0181C0, 0x080900–0x081DDF

  The data block at 0x0AA238–0x0AD1BF is the first candidate for the level description
  (Phase 4.4).
- The init copy table at 0x4000 (the C runtime start) and its source data are read at vblank 0.

## Open

- More sessions: other levels, the save/continue path, game over, cutscenes.
- Split "data" further: tilemaps the PPU reads through RAM, compared with tables read directly.
