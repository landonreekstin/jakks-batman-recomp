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
- **Playable**: `recomp/build/batman` (SDL2) runs the game natively at 60 Hz with sound,
  keyboard/gamepad and a persistent save file. The core runs about 16x faster than real time
  (5000 frames with rendering in 5.1 s).
- **Save states** `recomp/src/state.c`: ~32 KB snapshots taken at vblanks. A run that saves,
  loads in a new process and runs on is byte-identical to an uninterrupted run, and its
  frames still match MAME (`tests/state.sh`, checked at vblanks 1505, 3000 and 4321).
- **Display**: CRT filters `recomp/src/crt.c` (none, scanlines, aperture, composite, crt),
  CPU-only and threaded, 1280x960 output. Worst case (`crt`) is 4.9 ms per frame with 8 worker
  threads on this machine. They only read the finished frame. `osd.c` draws messages.
- **Mods** `recomp/src/mod.c` (format in `mods/README.md`, plugin API in `mod_api.h`): ROM text,
  word and file patches, RAM pokes, and native plugins with PC hooks. The ROM space is the full
  4 M words, with the upper half a separate copy (it was a mirror). Checks:
  - `example-text`, baked into a ROM, matches MAME;
  - `tests/mods/retf-identity` replaces 4944 RETFs through a hook and still matches MAME;
  - unmodded runs are unchanged.
- Not done: static recompiler (code is still interpreted). Only 5000 frames (~83 s) of play
  have been compared against MAME so far.
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
- `tests/state.sh <workdir> [save-at] [frames]` (env `INPUT`): save-state round trip, also
  compared with MAME when `<workdir>/mame` holds a capture with the same input.
- `tools/mame/capture.lua` makes the MAME reference. `tests/compare.py` does the diffs.
- `tools/ghidra/SeedAndExport.java`: headless Ghidra pass, exports the function list.

## Playing

`make -C recomp`, then `env -u LD_LIBRARY_PATH recomp/build/batman target/batman.bin`.
Keys: arrows, Z = A (jump), X = B (attack), C = X (search/use), Enter = Menu, Esc = quit.
The save goes to `target/batman.sav` (override with `--save`).
Save states: F5 save, F8 load, F6/F7 slot 0-9 (files `target/batman.st<N>`), hold Backspace to
rewind (up to 60 s). States only load into the same build and ROM image (SHA-256 checked).
Hold Tab to fast-forward (4x). F9 / Shift+F9 cycle the CRT filter (`--filter crt` at start),
F11 toggles fullscreen, F12 saves a screenshot of the filtered image to `target/batman-NNNN.png`.
`batman-rt --filter <mode>` applies a filter to `--snap` PNGs.

`--record tests/inputs/<name>.txt` writes the session as an input script, so real play
becomes a MAME comparison: `INPUT=tests/inputs/<name>.txt tests/check.sh <workdir> <vblanks>`
(the last line of the script gives the vblank count). Recordings start from a blank EEPROM,
like the MAME capture, and save to `<name>.txt.sav`. Loading a state or rewinding ends the
recording, since the script would no longer describe one uninterrupted run. Both captures write ~15 KB per vblank
(RAM + video RAM), so a 10-minute session is about 0.5 GB per side.

## Modding

See `mods/README.md`. Run `batman`/`batman-rt` with `--mods a,b`; the SDL build also reads
`mods/enabled.txt`. `MODS=a,b tests/check.sh` compares a data mod with MAME, which runs the ROM
file baked with `batman-rt --bake`. `batman-rt --romcov map.bin` (with `BATMAN_RENDER=1`) tags every ROM word by
who reads it; `tools/romcov.py` summarises and draws it. Findings are in `notes/rom-usage.md`:
so far nothing above word 0x100000 is ever read, so that space looks free for new content.
Next: more play sessions, then graphics, RAM map and level format.

## Tools / quirks

- Disassembly: `unidasm -arch unsp12 -xchbytes`. Ghidra: `unsp:LE:16:default`. naken_asm
  assembles only. MAME source is in `$MAME_SRC`.
- Devshell bug: `mame`/`unidasm` fail with "GLIBC_2.43 not found" because `LD_LIBRARY_PATH`
  points at an alsa-lib built against a newer glibc. Workaround: `env -u LD_LIBRARY_PATH mame ...`.
  The same variable breaks the SDL front end (sdl2-compat aborts with "Failed loading SDL3
  library"); the same workaround applies.
- MAME Lua memory taps are garbage-collected (and silently removed) unless kept referenced.
- MAME calls `frame_done` at t=0 only with `-debug`, so captures label frames by emulated time.
