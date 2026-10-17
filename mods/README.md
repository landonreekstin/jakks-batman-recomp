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
| `generate.py` (file) | if present, `make -C recomp` writes `mod.txt` from it and the ROM (for mods derived from the original art, which stay out of git) |
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
- `example-cheat`: infinite health and lives in the brawler levels (`poke 01e9 0020 always`,
  `poke 2099 0004 always`; see `notes/ram-map.md`).
- `example-level`: an edited Street Party layout. In the opening section the blue building is
  replaced by a stretch from further down the street (made with `tools/level.py`; see below).
  Checked against MAME over the first 6100 vblanks of `tests/inputs/session2.txt`.
- `robin`: work in progress. Robin's look (black hair, red tunic, green legs, yellow cape) and a
  staff: B hits in melee from 60 px instead of 40, with longer attack boxes, and `plugin.c` draws
  the staff. `generate.py` makes `mod.txt` from the ROM (`make -C recomp` runs it). The output holds
  edited copies of the original art, so it is gitignored. The staff (plugin, upper-half tiles) only
  works in our runtime. `notes/sprites.md` has the sprite and melee formats; `tools/sprites.py`
  draws every frame and exports/imports tiles.
- `example-lagmeter`: a plugin that hooks `wait_vblank` (0x005CDE) and counts lag frames on screen.
- `tests/mods/retf-identity`: a test plugin. It replaces a RETF with `api->retf`, and the
  game must still match MAME exactly.

## Editing level layouts

`tools/level.py` (format in `notes/level-format.md`) turns a scene's maps into JSON and back:

```
tools/level.py list    target/batman.bin                       # the 24 scenes
tools/level.py render  target/batman.bin 15 2 street.png       # whole map as an image
tools/level.py extract target/batman.bin 15 s15.json            # edit the "grid" rows (block bytes)
tools/level.py paste   s15.json 2 52 8 7 s15e.json              # or copy block columns
tools/level.py diff    target/batman.bin s15e.json my-mod > mods/my-mod/mod.txt
```

`diff` writes only the changed words, so a mod never carries the original ROM data. Keep the
extracted JSON out of git, like `target/`. Street Party is scene 15 and Fast Lane scene 16. A block
byte indexes the scene's block table, so any block already in that table can be placed anywhere.
