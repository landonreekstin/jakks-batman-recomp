*Created by `re new`. The rules for every RE project are in `../CLAUDE.md`.*

## Target

`target/batman.bin`: ROM dump of the JAKKS Pacific *The Batman* TV game (MAME set `jak_batm`,
confirmed with `mame -verifyroms`).
- Size 4 MiB (0x400000)
- SHA-256 `4ec5438d16e222cc624820f45579071b8f8a4e91c65ae76e5425565509271766`
- SHA-1 `5875d57bb3fe0cac5d20e626e4f82a0e5f9bb94c`, CRC32 `46f848e5`

Question: can the game be recompiled to native x86 Linux, and later ported to a console
that has an openFPGA core on the Analogue Pocket (GBA is the best candidate)?

Architecture: Sunplus SPG2xx SoC with a µ'nSP CPU (16-bit, word-addressed). The ROM is
little-endian 16-bit words, so word address A = file offset 2·A. Details are in
`notes/rom-layout.md`.

## Status

Approach: a faithful native runtime first (interpreter + SPG24x peripheral models, checked
against MAME). The static recompiler will be checked against that runtime, and so will the
PPU/SPU work.

- **CPU core** `recomp/src/unsp.c`: µ'nSP ISA 1.0 with MAME's semantics and cycle counts.
  It matches MAME's instruction trace exactly: 1,832,220 instructions over 20 frames, IRQ
  timing included.
- **Machine** `recomp/src/spg.c`: memory map, PPU registers/IRQ, GPIO, DS register, PRNG,
  and MAME-identical scheduling (vblank, scanline counter, 50 Hz sound-stream updates,
  1234 Hz PRNG clock). `i2c.c` is the 24C04 save EEPROM. `spu.c` is the 16-channel SPU and
  `ppu.c` the tile/sprite renderer, both ported from MAME with their quirks kept.
- **Checked against MAME, fully standalone** (no replay), with
  `tests/inputs/level1-start.txt`: title → New Game → Easy → save to slot A → first level,
  walking and attacking. All 5000 frames are identical (RAM + video RAM), all 798,387
  peripheral writes and 133,165 reads (values included) match, and the rendered frames are
  pixel-identical to MAME snapshots. The audio follows MAME's `-wavwrite` loudness envelope
  with correlation 0.998.
- Not done: SDL front end, static recompiler. Only 5000 frames (~83 s) of play have been
  compared so far.
- Game structure is in `notes/game-structure.md`; ROM layout in `notes/rom-layout.md`.

## How to check

- `tests/check.sh <workdir> [frames] [--trace]`: captures MAME (cached in `<workdir>/mame`),
  runs `recomp/build/batman-rt`, then compares frames, I/O writes and the instruction trace.
  Env: `INPUT=tests/inputs/<x>.txt` (controller script), `SNAPS=a,b,c` (pixel-compare those
  vblanks), `REPLAY=1` (feed MAME's peripheral reads back in).
- Runner: `recomp/build/batman-rt target/batman.bin --frames N [--input f] [--snap dir
  --snap-frames a,b] [--wav out.wav] [--dump f] [--iolog f] [--trace f]`.
  `tools/contact_sheet.py` tiles snapshots into one PNG for a quick look. Keep `--trace` to ~20 frames
  (about 93k instructions per frame).
- `tools/mame/capture.lua` makes the MAME reference. `tests/compare.py` does the diffs.
- `tools/ghidra/SeedAndExport.java`: headless Ghidra pass, exports the function list.

## Tools / quirks

- Disassembly: `unidasm -arch unsp12 -xchbytes`. Ghidra: `unsp:LE:16:default`. naken_asm
  assembles only. MAME source is in `$MAME_SRC`.
- Devshell bug: `mame`/`unidasm` fail with "GLIBC_2.43 not found" because `LD_LIBRARY_PATH`
  points at an alsa-lib built against a newer glibc. Workaround: `env -u LD_LIBRARY_PATH mame ...`.
- MAME Lua memory taps are garbage-collected (and silently removed) unless kept referenced.
- MAME calls `frame_done` at t=0 only with `-debug`, so captures label frames by emulated time.
