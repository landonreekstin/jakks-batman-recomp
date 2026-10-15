/* SPG24x machine for jak_batm: memory map, peripherals and MAME-equivalent timing.
 * Register behaviour follows MAME 0.287 src/devices/machine/spg2xx*.cpp. */
#ifndef SPG_H
#define SPG_H

#include <stdint.h>
#include <stdio.h>

#include "i2c.h"
#include "unsp.h"

#define SPG_CLOCK 27000000

/* one peripheral access, as logged by tools/mame/capture.lua */
typedef struct io_rec {
    uint32_t frame;
    uint16_t addr, data;
    char rw;
} io_rec_t;

typedef struct spg {
    unsp_t cpu;

    const uint16_t *rom;   /* 0x200000 words, mapped at 0 and mirrored at 0x200000 */
    uint16_t ram[0x2800];
    uint16_t vregs[0x100];  /* 0x2800 PPU */
    uint16_t vram[0x700];   /* 0x2900 scroll, 0x2a00 hcomp, 0x2b00 palette, 0x2c00 sprites */
    uint16_t aregs[0x800];  /* 0x3000 SPU, stored only */
    uint16_t ioregs[0x100]; /* 0x3d00 */
    uint16_t dmaregs[4];    /* 0x3e00 */

    uint16_t buttons;       /* port A, bits 8..15 active high */
    eeprom_t eeprom;        /* 24C04 on port C: SCL = bit 1, SDA = bit 0 */

    /* timing (see spg.c) */
    uint32_t frame;          /* vblank-begin events seen so far */
    int64_t next_vbl_begin;  /* event is seen once cpu.cycles > this */
    int64_t next_vbl_end;
    uint32_t vbl_end_count;
    unsigned __int128 vblank_start_as;

    /* replay: peripheral reads return MAME's recorded values */
    io_rec_t *replay;
    size_t replay_len, replay_pos;
    int replay_diverged;
    uint32_t model_mismatch[0x1800]; /* per address 0x2800-0x3fff: model value != recorded value */

    FILE *iolog;             /* our own accesses, same format as capture.lua (frame labels start at 1) */
    void (*on_frame)(struct spg *m);
} spg_t;

int spg_init(spg_t *m, const uint16_t *rom);
void spg_reset(spg_t *m);
/* Runs until the next vblank begins (frame boundary as MAME sees it). Returns 0 on CPU error. */
int spg_run_frame(spg_t *m, FILE *trace, uint64_t *trace_budget);
int spg_load_replay(spg_t *m, const char *path);
void spg_trace_line(spg_t *m, FILE *f);

#endif
