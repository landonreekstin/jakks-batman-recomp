/* SPG2xx SPU: 16 wavetable channels (8/16-bit PCM, IMA ADPCM, ADPCM36), envelopes, rampdown.
 * Port of MAME 0.287 src/devices/machine/spg2xx_audio.cpp (BSD-3-Clause, Ryan Holtz,
 * Jonathan Gevaryahu), including its quirks, so that channel status matches MAME. */
#ifndef SPU_H
#define SPU_H

#include <stdint.h>

#define SPU_RATE 70312 /* MAME: stream_alloc(0, 2, 281250/4) */

typedef struct spu {
    uint16_t regs[0x200];       /* 0x3000 per-channel */
    uint16_t phase_regs[0x200]; /* 0x3200 per-channel phase */
    uint16_t ctrl[0x400];       /* 0x3400 control */

    uint8_t sample_shift[16];
    uint32_t sample_count[16];
    double channel_rate[16];
    double channel_rate_accum[16];
    uint32_t rampdown_frame[16];
    uint32_t envclk_frame[16];
    uint32_t envelope_addr[16];
    uint16_t curr_beat_base_count;

    struct { int32_t signal, step; } adpcm[16];
    struct { uint16_t remaining, header; int16_t prevsamp[2]; } adpcm36[16];

    uint64_t samples_done;      /* stream write position */

    void *ctx;
    uint16_t (*read)(void *ctx, uint32_t addr);
    /* optional sink for generated stereo samples */
    void (*out)(void *ctx, int16_t l, int16_t r);
} spu_t;

void spu_reset(spu_t *s);
uint16_t spu_read(spu_t *s, uint32_t off);           /* off = addr - 0x3000 */
void spu_write(spu_t *s, uint32_t off, uint16_t data);
void spu_generate(spu_t *s, uint32_t samples);

#endif
