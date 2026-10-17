# Level format

How the background maps of every scene are stored and drawn. Found by:
- tracing the code that writes the page-2 tilemap (`batman-rt --watch`, then `--trace` from a save
  state);
- reading that code with `unidasm`.

`tools/level.py` implements the decoding. Word addresses throughout.

## Checks

- **Live tilemaps.** `level.py check` compares the decoded maps with the tilemap and attribute RAM in
  `--vdump` dumps of `session1`.
  - Street Party (scene 15): 124 dumps, every 300 vblanks.
  - Fast Lane (scene 16): 43 dumps.

  Every tile column on screen, on both pages, equals a column of the decoded map: tile number and
  attribute byte, on every visible row (Street Party page 2: 5043 of 5043). Most sit exactly where
  the game's section remap (below) puts them. The rest were written under an earlier section
  state, and the check finds them elsewhere in the map.
- **Round trip.** `level.py extract` then `level.py build` reproduces the data of all 24 scenes
  word for word. The map words are re-encoded from the JSON grid and table; the blocks not yet
  decoded are copied raw.

## Scene table: 0x0D869E

- One count word (24), then 24 records of 24 words: 12 far pointers stored as (low word, segment).
- A scene's data is contiguous: it runs from its palette up to the next scene's palette. The last
  scene's data ends at the table.

| Slot | Contents |
|---|---|
| 0 | tileset graphics address. 0x080000 in every scene, so the tile number base is 0x8000 |
| 1 | background palette: 128 words, banks 0–7 (colour 0 is made transparent) |
| 2 | page 2 map (the playfield) |
| 3 | page 1 map (parallax background), or 0 for none |
| 4–11 | not decoded yet. Slot 5's pointer is copied into the layer struct at +0x12; for Street Party it is 1200 words, one per tile column of the map, probably floor or collision. Enemy/object placement is probably among the others |

`FUN_00c917(id)` copies record `id` to RAM 0x080E. `FUN_00bfb5(layer, id, x, y)` sets up a layer
struct from it:
- page 1's struct is at 0x25E9, page 2's at 0x2603;
- struct +0x0B/+0x0C hold the map pointer, +0x0A the width in blocks, +0x09 the tile base;
- +0x10/+0x11 hold the maximum camera X/Y, which are (width·4 − 40)·8 and (height·4 − 30)·8.

## Map

```
word 0        width  (32x32 blocks)
word 1        height (blocks)
word 2        n, the number of block kinds
words 3..     n table entries: block offsets into the global pools
then          height rows of width/2 words, two block bytes per word (low byte = even column)
```

- A block byte b selects table entry b. The entry e is an offset into two **global** pools, whose
  addresses are constants in the code:
  - tiles at 0x09EC10 + e: 16 words, a 4×4 grid of tile numbers in row-major order. The scene's
    tile base (0x8000) is added to each one;
  - attributes at 0x0A6580 + e/2: 8 words, 4 rows of 2 words, one attribute byte per tile (low byte
    = even column). The byte holds the palette bank in bits 0–3, flip x in bit 4 and flip y in bit 5.
- Block kinds per scene go up to 168, which leaves room for more in a byte.
- Every scene shares one tile and block pool. A new layout can use any block of any scene, as long
  as it has a table entry and its colours exist in the scene's palette.

## How the game draws it (FUN_00c333 → c3e4 tiles, c512 attributes)

- **Columns.** The tilemap is a 64×32 ring. When the camera moves, the game builds one column of 36
  rows into RAM 0x080E: 9 block rows, starting at the camera's tile row.
  - It then copies 31 of those rows into the ring (FUN_0dc620).
  - The 9th block row reads past the end of the map, into whatever follows it. At camera Y ≥ 8 that
    garbage lands in ring row 0, under the HUD, and stays there.
- **Rows.** FUN_00c471 builds rows for vertical scrolling the same way.
- **Section remap (FUN_00e74e).** For page 2 the camera's tile column first goes through the section
  struct pointed to by RAM 0x270B (0x0150 in Street Party):
  `col = x < S[1] ? x : (x − S[1]) mod S[0] + S[7]`.
  - The camera position is therefore virtual. A section is a stretch of S[0] tile columns starting
    at map column S[7].
  - The stretch repeats as a treadmill while the player fights. When it's cleared the next section
    takes over, and S[8] (RAM 0x0158) counts the sections.
  - Street Party has 12 sections (S[8] = 0..0x0B). Fast Lane loops the same way.
  - Where the section list comes from in ROM is still open. It is probably one of slots 4–11.

## Open

- Slots 4–11: collision or floor, the section list, enemy spawns and triggers.
- How scene ids map to levels. Street Party's playfield is scene 15 and Fast Lane's is scene 16,
  while RAM 0x015A holds the level ids 0x0C and 0x0D.
- Palette banks 6–7 differ at runtime from slot 1's ROM data, so something else loads them.
