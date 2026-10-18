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
- **A second session matches MAME**: `tests/inputs/session2.txt` (11,760 vblanks), Street Party
  played to Game Over. All frames are identical, all writes and reads match, and 3/3 snapshots are
  identical.
- **A real play session matches MAME**: `tests/inputs/session1.txt`, recorded by the user with
  `--record`. It covers 68,520 vblanks (19 min): the Street Party brawler level, then the
  Fast Lane Batcycle level to Level Completed. All frames are identical, all 20,586,666 peripheral writes and
  541,817 reads match, and 14/14 snapshots are pixel-identical. Each side of the capture is about 1.7 GB.
- **Co-op prototype** (`recomp/src/coop.c`): local two-player brawler and detective levels (Street Party and *Search for a Smile* checked; P2 = Robin
  with `mods/robin-p2`). Driving levels stay single player. Single player is unchanged, and session2 still matches MAME. Co-op runs repeat
  identically and survive save/load.
- **Netplay** (`recomp/src/net.c`, `notes/netplay.md`): online co-op, host = P1, guest = P2, with
  rollback. The guest joins from the host's state at any time, and desyncs are found by a state hash
  every second and fixed by resending the host's state. `tests/net.sh` (loopback with simulated
  latency, jitter and loss): host and guest end identically, the settled inputs replay offline to
  the same state, and a knocked-off guest is resynced. Not yet tried between two machines. Next: P1
  respawning in place while P2 lives, and the driving levels (plan in `notes/coop.md`).
- Not done: static recompiler (code is still interpreted).
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
- `tests/net.sh <workdir> [start] [frames] [lat,jitter,loss%]`: netplay over loopback, in real time
  (see Status).
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
`-` / `=` (or keypad −/+) change the volume in 10% steps and M mutes. The volume and the CRT filter are
remembered in `target/batman.cfg`, and `--volume 0-100` sets it at start. It only scales the output, so the
emulation is unchanged.
`batman-rt --filter <mode>` applies a filter to `--snap` PNGs.

**Co-op** (`--coop`, `recomp/src/coop.c`, `notes/coop.md`): two players in the brawler levels. P2 uses
W A S D, J = A, K = B, L = X, P = Menu, or the second controller (or the only one, while P1 keeps the
keyboard). Add `--mods robin-p2` to make P2 Robin. P2 is a second copy of the player's state, swapped into
RAM around a second run of the player update. It also takes part in the enemies' targeting, the collision
pass and the HUD (at the bottom of the screen), and has its own lives and respawn. The camera follows the
midpoint, with a leash. Native only. Input scripts take an optional second mask for P2.

**Online** (`notes/netplay.md`): the host runs `batman target/batman.bin --mods robin-p2 --host`
(port 7845, TCP and UDP; `--host 9000` for another) and plays as P1. Player 2 runs
`batman target/batman.bin --mods robin-p2 --join <host address>[:port]` and joins the host's game as
Robin, from wherever it is. Both need the same build, ROM and mods. Each player uses P1's keys or a
controller. `--net-delay N` sets the input delay (2 frames). The status line at the bottom shows the
ping and the last rollback. Rewind, state loads, fast forward and recording are off while connected.
The guest's save file is left alone. The guest keeps retrying until the host is up (and after the host quits); the host listens again when player 2 leaves. `batman-rt --host/--join` does the same headless (for
`tests/net.sh`).

**Packaged** in nixos-config (`pkgs/batman`, pinned to a commit of this repo on GitHub, and
`modules/nixos/programs/batman.nix`): `batman [play|coop|host|join [ip]]` plus start-menu
entries. The launcher downloads the ROM from the NAS public drop on first run and generates
Robin's sprites into `~/.local/share/batman`. gaming-pc hosts. Blaney's blaney-pc joins
`192.168.1.76:7845`, which optiplex-fw forwards to gaming-pc over wg-nas. Hosts that play
together must be on the same pin, so after changing netplay or the game, push here and bump
the pin (nixos-config `docs/companion-repos.md`).

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
Graphics are decoded (`notes/graphics.md`, `tools/gfx.py`, which rebuilds screens
pixel-identically from `batman-rt --vdump` dumps). Game variables are in `notes/ram-map.md`
(`tools/ramsearch.py`): level id, health, lives, special meter, player X, camera, brawler sections.
The background map format is decoded (`notes/level-format.md`). `tools/level.py` lists the 24
scenes, renders whole maps to PNG, and extracts/builds them as JSON. Rebuilding reproduces all 24
scenes word for word, and every on-screen tile column in both levels of `session1` matches the
decoded maps. `batman-rt --watch out.txt w:LO-HI,r:LO-HI,from:N,to:N` logs CPU writes and data reads
with their PC, for tracing data back to code. `mods/example-level` (an edited
Street Party, made with `level.py paste`/`diff`) matches MAME running the baked ROM. The player's sprite
frames are decoded (`notes/sprites.md`, `tools/sprites.py`). `mods/robin` turns Batman into Robin, rebuilt from the
staff fighter's frames (`generate.py`), with its staff attacks, plus a faster, longer Birdarang. It is
all data and matches MAME. The sprite bank, animation table and melee/Batarang code are in
`notes/sprites.md`. Also open: the undecoded scene slots (collision, sections, enemies).

## Tools / quirks

- Disassembly: `unidasm -arch unsp12 -xchbytes`. Ghidra: `unsp:LE:16:default`. naken_asm
  assembles only. MAME source is in `$MAME_SRC`.
- Devshell bug: `mame`/`unidasm` fail with "GLIBC_2.43 not found" because `LD_LIBRARY_PATH`
  points at an alsa-lib built against a newer glibc. Workaround: `env -u LD_LIBRARY_PATH mame ...`.
  The same variable breaks the SDL front end (sdl2-compat aborts with "Failed loading SDL3
  library"); the same workaround applies.
- MAME Lua memory taps are garbage-collected (and silently removed) unless kept referenced.
- MAME calls `frame_done` at t=0 only with `-debug`, so captures label frames by emulated time.
