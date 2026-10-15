/* SPG2xx PPU renderer: two tile/linemap pages + 256 sprites, 320x240 RGB555 -> XRGB8888.
 * Port of MAME 0.287 src/devices/machine/spg_renderer.cpp and spg2xx_video.cpp
 * screen_update (BSD-3-Clause, David Haywood, Ryan Holtz). */
#ifndef SPG_PPU_H
#define SPG_PPU_H

#include <stdint.h>

#define PPU_W 320
#define PPU_H 240

typedef struct ppu {
    uint32_t rgb555_to_rgb888[0x8000];
    uint32_t lut[0x8000];        /* with saturation and fade applied */
    uint16_t lut_30, lut_3c;     /* register values the lut was built for */
    int lut_valid;
    uint32_t ycmp_table[480];    /* rebuilt only when 0x281c-0x281e are written, as in MAME */
    uint16_t linebuf[PPU_W];
} ppu_t;

/* video memory the renderer reads, supplied by the machine */
typedef struct ppu_mem {
    const uint16_t *vregs;    /* 0x2800 */
    const uint16_t *scroll;   /* 0x2900 */
    const uint16_t *palette;  /* 0x2b00 */
    const uint16_t *sprites;  /* 0x2c00 */
    void *ctx;
    uint16_t (*read)(void *ctx, uint32_t addr); /* side-effect-free bus read */
} ppu_mem_t;

void ppu_init(ppu_t *p);
/* call on writes to 0x281c, 0x281d, 0x281e */
void ppu_update_vcmp(ppu_t *p, uint16_t r1c, uint16_t r1d, uint16_t r1e);
void ppu_render(ppu_t *p, const ppu_mem_t *mem, uint32_t *out /* PPU_W * PPU_H */);

#endif
