# Mods

Each mod is a directory with a `mod.txt`. Mods are applied when the game starts; the ROM file
in `target/` is never changed.

```
env -u LD_LIBRARY_PATH recomp/build/batman target/batman.bin --mods example-text,example-lagmeter
```

To load mods without passing `--mods`, list them one per line in `mods/enabled.txt`.
Save states remember the mod set and won't load under a different one.

## mod.txt

Addresses are µ'nSP word addresses (segment << 16 | offset). The ROM file's byte offset is
2 × address. Numbers are hex, and `#` starts a comment.

| Directive | Effect |
|---|---|
| `name <text>` | display name |
| `rom <sha256>` | required: the original `batman.bin` the mod was made for |
| `text <addr> "string"` | ROM words ← characters (the game stores text one character per word) |
| `words <addr> <w> ...` | ROM words ← the values |
| `patch <addr> <file>` | ROM words ← a file of little-endian words |
| `poke <addr> <value> [always]` | RAM (< 0x2800) ← value after reset, or at every vblank |
| `plugin <file.so>` | native C plugin, built from `plugin.c` by `make -C recomp` (API: `recomp/src/mod_api.h`) |

The ROM space runs from 0x000000 to 0x3FFFFF. The upper half starts as a copy of the
file and can be patched on its own, which makes it room for new content. Content placed
there can't be reproduced in MAME, which mirrors the file into the upper half.

Two mods that patch the same words are reported. The later one in the list wins.

## Checking a data mod against MAME

A mod with only `text`, `words` and `patch` lines (patching the lower half) is a plain ROM
change. MAME can run it, so it can be checked like the unmodded game:

```
MODS=example-text INPUT=tests/inputs/level1-start.txt SNAPS=1500 tests/check.sh /tmp/chk 2600
```

`batman-rt --mods <m> --bake out.bin` writes the patched ROM file.

## Examples

- `example-text`: renames "New Game" on the title menu. Checked against MAME.
- `example-cheat`: infinite health in the brawler levels (`poke 01e9 0020 always`; see
  `notes/ram-map.md`).
- `example-lagmeter`: a plugin that hooks `wait_vblank` (0x005CDE) and counts lag frames on screen.
- `tests/mods/retf-identity`: a test plugin. It replaces a RETF with `api->retf`, and the
  game must still match MAME exactly.
