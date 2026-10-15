# Game structure (from MAME traces + disassembly)

Addresses are µ'nSP word addresses (segment << 16 | offset).

## Control flow
- `reset` 0x5DCD: C runtime start. `sp = 27ff`, copies init data from a table at 0x4000
  (count, then dst, src segment → SR, src offset, length per entry), calls `main` 0x0DBFA8,
  then `jmp $`.
- `irq_handler` 0x5CD0 (IRQ0–7 all point here): acknowledges `[0x2863]`. On vblank (bit 0)
  it increments the frame counter `[0x0809]`, then `reti`. Nothing else runs in interrupts.
- `wait_vblank` 0x5CDE: spins until `[0x0809]` changes. The game is a plain main loop
  paced by this function, so it doesn't depend on cycle-exact timing.
- FIQ/BREAK 0x5CCE: stub (`fiq off; reti`).
- 0x0DC7C4: PAL/NTSC detection at boot. It reads the scanline counter 0x3D1C until it
  wraps, then stores 60 (max line ≤ 300) or 50 at `[0x2090]`.

## Functions
- Ghidra (unsp:LE:16:default, seeded at the vectors + main) finds 778 functions in the low
  2 MiB: segments 0x00 (334), 0x06 (57), 0x07 (103) and 0x0D (284). List in
  `evidence/ghidra-functions.tsv` (word addresses). The bodies look compiler-generated:
  bp frames, arguments on the stack, `retf`.
- Segment 0x0D also holds the I/O drivers: port C I2C bit-banging around 0x0D9950–0x0D99E0,
  port A input read at 0x0DD787.

## Peripheral use (1500 frames, boot → title)
- IRQs: only the PPU vblank IRQ (`0x2862` = 1, later 3; the screen-position IRQ registers are
  never set). I/O IRQs stay disabled (`0x3D21` = 0). No timers, no UART, no ADC reads,
  no sprite DMA, no system DMA.
- Busiest registers: sprite RAM 0x2C00+, palette 0x2B00+, SPU 0x30xx–0x34xx, `0x3D2F` (DS),
  port C (I2C).
- Inputs: port A bits 8–15, active high (MAME `batman` input port).
- Save: 24C04 I2C EEPROM on port C (SCL bit 1, SDA bit 0), blank = 0xFF.
- SPU status reads: `0x340F` (channel playing) and `0x340B` (channel stop) are polled by
  the sound driver (0x00EA92, 0x00EAC6).
