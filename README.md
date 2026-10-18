# The Batman, natively

A native Linux runtime for the JAKKS Pacific *The Batman* TV game (2004, Sunplus SPG2xx,
µ'nSP CPU; MAME set `jak_batm`). It runs the original game code in a µ'nSP interpreter and
models the console's hardware: video, sound, timers, controller and save EEPROM. Every frame
matches MAME exactly over long play sessions. On top of that it adds:
- save states, rewind and CRT filters;
- mods;
- local and online two-player co-op, with Robin as player 2 and rollback netcode.

**No game data is included.** You need your own dump of the ROM: 4 MiB, SHA-256
`4ec5438d16e222cc624820f45579071b8f8a4e91c65ae76e5425565509271766`. The Robin sprites are
generated from it at build time (`mods/robin/generate.py`).

## Build and play

```sh
make -C recomp                     # needs gcc, SDL2, pkg-config, python3
recomp/build/batman batman.bin      # arrows, Z jump, X attack, C search, Enter menu
```

Two players on one PC: add `--coop --mods robin-p2`. Player 2 uses W A S D, J K L and P, or a
second controller.

Online:
- the host runs `recomp/build/batman batman.bin --mods robin-p2 --host` (TCP and UDP port 7845);
- the friend runs `recomp/build/batman batman.bin --mods robin-p2 --join <host address>` and
  joins the host's game, wherever it is.

Both need the same build, ROM and mods. The friend's window waits, and joins again by itself, until the
host is there.

## More

- `CLAUDE.md`: status, how everything is checked against MAME, and all options.
- `notes/`: the reverse-engineering notes (hardware, game structure, RAM map, graphics, levels,
  sprites, co-op, netplay).
