# ROM layout — batman.bin

How to read the ROM: it is little-endian 16-bit words, and CPU word address A is at file
offset 2·A.

## Vectors (words 0xFFF5–0xFFFF, file 0x1FFEA)
| Vector | Target |
|---|---|
| BREAK, FIQ | 0x5CCE (`f14c` then `9a98` RETI: a stub) |
| RESET | 0x5DCD (`9108 27FF` = SP=0x27FF, the top of internal RAM) |
| IRQ0–7 | 0x5CD0 (push, then reads `[0x2863]` = PPU IRQ status) |

## Reset = C runtime startup (unidasm, confirmed)
`sp = 27ff`, then walks a copy table at word 0x4000 (count, then per entry dst, src
segment→SR, src offset, length) and copies init data into RAM with `ds:[r4++]`. After that
it does `call 0dbfa8` (main) and a `jmp $` spin. The functions use bp frames with stack
args, so this is compiled C (Sunplus unSP IDE toolchain, presumably).

## MAME (0.287, `$MAME_SRC/src/mame/tvgames/spg2xx_jakks.cpp`)
- Set `jak_batm`: "The Batman (JAKKS Pacific TV Game)", 2004, JAKKS Pacific / HotGen Ltd,
  flagged MACHINE_IMPERFECT_SOUND. The ROM CRC/SHA-1 match (`-verifyroms`: good).
- Machine `spg2xx_jakks`: SPG24X at 27 MHz, `I2C_24C04` EEPROM on port C (SCL = bit 1,
  SDA = bit 0), `mem_map_2m`: the whole 4 MiB file is mapped at word 0x000000–0x1FFFFF
  and mirrored at 0x200000.
- Boots headless to the title screen (New Game / Continue) in about 25 s of emulated time.

## SPG2xx memory map (from MAME / public docs, not yet verified on this unit)
- 0x0000–0x27FF RAM
- 0x2800–0x3FFF I/O: PPU 0x28xx, SPU 0x30xx–0x37xx, system 0x3Dxx
- ROM above that; banked through 0x3D2x (exact scheme still unknown)

## Content
- The two 2 MiB halves are identical except bytes 0x0000–0x7FFB (words 0x0000–0x3FFD).
  RAM/I/O shadows that range in the low copy, so the CPU never sees it there. It *is*
  visible in the high copy at words 0x100000–0x103FFD (MAME maps the full 4 MiB).
- Code (rough heuristic: far-call targets and RETF counts) sits mostly in segments 0x00,
  0x06, 0x07 and 0x0D (segment = 64K words), with about 775 distinct call targets.
  `main` = 0x0DBFA8. Banks 0x08–0x09 are mostly 0x7777 (4bpp
  graphics). Bank 0x0E is mostly 0x80FF.
- Counts in the first half: 805 RETF (`9a90`), 30 RETI (`9a98`).
- Plain-ASCII strings: menu text ("BATMAN 1", "No saved games", "Continue") and Arkham
  dialogue `*{...}`.

## Unknowns
- Exact SoC variant: MAME uses SPG24X
- Banking scheme and mirror behaviour of the 4 MiB part
- Save device: MAME says 24C04 (512 bytes); save format still unknown
- What the differing bytes 0x0000–0x7FFB are, and whether the game ever reads words 0x100000+
  (watch with a MAME read watchpoint)
