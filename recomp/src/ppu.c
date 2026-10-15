/* SPG2xx PPU renderer. See ppu.h. Arithmetic follows MAME, including its integer widths. */
#include "ppu.h"

#include <math.h>
#include <string.h>

#define RD(a) (mem->read(mem->ctx, (a) & 0x3fffff))

static const uint8_t blend_levels[4] = { 0x08, 0x10, 0x18, 0x20 };

void ppu_init(ppu_t *p)
{
    uint8_t rgb5_to_rgb8[32];
    for (int i = 0; i < 32; i++) rgb5_to_rgb8[i] = (uint8_t)((i << 3) | (i >> 2));
    for (int i = 0; i < 0x8000; i++)
        p->rgb555_to_rgb888[i] = ((uint32_t)rgb5_to_rgb8[(i >> 10) & 0x1f] << 16) |
                                 ((uint32_t)rgb5_to_rgb8[(i >> 5) & 0x1f] << 8) |
                                 rgb5_to_rgb8[i & 0x1f];
    p->lut_valid = 0;
    for (int i = 0; i < 480; i++) p->ycmp_table[i] = 0xffffffff;
}

void ppu_update_vcmp(ppu_t *p, uint16_t r1c, uint16_t r1d, uint16_t r1e)
{
    /* "not correct when step is used" according to MAME */
    int currentline = 0;
    int step = r1e & 0xff;
    if (step & 0x80) step -= 0x100;
    int inc = r1c << 4;
    int counter = 0;
    for (int i = 0; i < 480; i++) {
        if (i < r1d) {
            p->ycmp_table[i] = 0xffffffff;
        } else {
            if (currentline >= 0 && currentline < 256) p->ycmp_table[i] = (uint32_t)currentline;
            counter += inc;
            while (counter >= (0x20 << 4)) {
                currentline++;
                inc += step;
                counter -= 0x20 << 4;
            }
        }
    }
}

static void update_palette_lookup(ppu_t *p, uint16_t r30, uint16_t r3c)
{
    if (p->lut_valid && p->lut_30 == r30 && p->lut_3c == r3c) return;
    const float u8_to_float = 1.0f / 255.0f;
    const float sat_adjust = (0xff - (r3c & 0x00ff)) / (float)(0xff - 0x20);
    for (int i = 0; i < 0x8000; i++) {
        uint32_t src = p->rgb555_to_rgb888[i];
        if ((r3c & 0x00ff) != 0x0020) { /* saturation */
            const float r = (uint8_t)(src >> 16) * u8_to_float;
            const float g = (uint8_t)(src >> 8) * u8_to_float;
            const float b = (uint8_t)src * u8_to_float;
            const float luma = r * 0.299f + g * 0.587f + b * 0.114f;
            const int ir = (int)floor((luma + (r - luma) * sat_adjust) * 255.0f);
            const int ig = (int)floor((luma + (g - luma) * sat_adjust) * 255.0f);
            const int ib = (int)floor((luma + (b - luma) * sat_adjust) * 255.0f);
            src = (ir > 255 ? 0xff0000u : (ir < 0 ? 0 : (uint32_t)(uint8_t)ir << 16)) |
                  (ig > 255 ? 0x00ff00u : (ig < 0 ? 0 : (uint32_t)(uint8_t)ig << 8)) |
                  (ib > 255 ? 0x0000ffu : (ib < 0 ? 0 : (uint32_t)(uint8_t)ib));
        }
        if (r30 != 0) { /* fade */
            const uint8_t sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
            const uint8_t r = (uint8_t)(sr - r30), g = (uint8_t)(sg - r30), b = (uint8_t)(sb - r30);
            src = (r > sr ? 0 : (uint32_t)r << 16) | (g > sg ? 0 : (uint32_t)g << 8) | (b > sb ? 0 : b);
        }
        p->lut[i] = src;
    }
    p->lut_30 = r30;
    p->lut_3c = r3c;
    p->lut_valid = 1;
}

static uint8_t mix_channel(uint8_t bottom, uint8_t top, uint8_t alpha)
{
    return (uint8_t)(((0x20 - alpha) * bottom + alpha * top) >> 5);
}

static void draw_tilestrip(ppu_t *p, const ppu_mem_t *mem, uint32_t screenwidth, uint32_t drawwidthmask,
                           int blend, int flip_x, uint32_t tile_h, uint32_t tile_w, uint32_t gfx_addr,
                           uint32_t tile, uint32_t tile_scanline, int drawx, int flip_y,
                           uint32_t palette_offset, uint32_t nc_bpp, uint32_t bits_per_row,
                           uint32_t words_per_tile, uint8_t blendlevel)
{
    const uint32_t yflipmask = flip_y ? tile_h - 1 : 0;
    uint32_t m = gfx_addr + words_per_tile * tile + bits_per_row * (tile_scanline ^ yflipmask);
    uint32_t bits = 0, nbits = 0;

    for (int32_t i = 0; i < (int32_t)tile_w; i++) {
        const int32_t x = flip_x ? (int32_t)tile_w - 1 - i : i;
        const int realdrawpos = (int)((uint32_t)(drawx + x) & drawwidthmask);

        bits <<= nc_bpp;
        if (nbits < nc_bpp) {
            uint16_t b = RD(m++);
            b = (uint16_t)((b << 8) | (b >> 8));
            bits |= (uint32_t)b << (nc_bpp - nbits);
            nbits += 16;
        }
        nbits -= nc_bpp;
        const uint32_t pal = palette_offset + (bits >> 16);
        bits &= 0xffff;

        if (realdrawpos >= 0 && realdrawpos < (int)screenwidth) {
            const uint16_t rgb = mem->palette[pal & 0xff];
            if (!(rgb & 0x8000)) {
                uint16_t *px = &p->linebuf[realdrawpos];
                if (blend && !(*px & 0x8000))
                    *px = (uint16_t)((mix_channel((*px >> 10) & 0x1f, (rgb >> 10) & 0x1f, blendlevel) << 10) |
                                     (mix_channel((*px >> 5) & 0x1f, (rgb >> 5) & 0x1f, blendlevel) << 5) |
                                     mix_channel(*px & 0x1f, rgb & 0x1f, blendlevel));
                else
                    *px = rgb;
            }
        }
    }
}

static void draw_linemap(ppu_t *p, const ppu_mem_t *mem, uint32_t scanline, uint32_t gfx_addr,
                         const uint16_t *scrollregs, const uint16_t *tilemapregs)
{
    (void)gfx_addr;
    if (scanline >= 240) return;
    const uint32_t tilemap = tilemapregs[2], palette_map = tilemapregs[3];
    const int realline = (int)((scanline + scrollregs[1]) & 0xff);
    const uint32_t tile = RD(tilemap + realline);
    uint16_t palette = RD(palette_map + realline / 2);
    if (scanline & 1) palette >>= 8;
    else palette &= 0x00ff;
    int sourcebase = (int)(tile | ((uint32_t)palette << 16));
    const uint32_t ctrl = tilemapregs[1];

    if (ctrl & 0x80) { /* HiColor */
        for (int i = 0; i < 320; i++) {
            const uint16_t data = RD((uint32_t)(sourcebase + i));
            if (!(data & 0x8000)) p->linebuf[i] = data & 0x7fff;
        }
        return;
    }
    const uint32_t attr = tilemapregs[0];
    const uint32_t nc_bpp = ((attr & 3) + 1) << 1;
    uint32_t palette_offset = (attr & 0x0f00) >> 4;
    palette_offset >>= nc_bpp;
    palette_offset <<= nc_bpp;
    uint32_t bits = 0, nbits = 0;
    for (int i = 0; i < 320; i++) {
        bits <<= nc_bpp;
        if (nbits < nc_bpp) {
            uint16_t b = RD((uint32_t)sourcebase++);
            b = (uint16_t)((b << 8) | (b >> 8));
            bits |= (uint32_t)b << (nc_bpp - nbits);
            nbits += 16;
        }
        nbits -= nc_bpp;
        const uint32_t pal = palette_offset + (bits >> 16);
        bits &= 0xffff;
        const uint16_t rgb = mem->palette[pal & 0xff];
        if (!(rgb & 0x8000)) p->linebuf[i] = rgb;
    }
}

static void draw_page(ppu_t *p, const ppu_mem_t *mem, uint32_t scanline, int priority, uint16_t gfx_seg,
                      const uint16_t *scrollregs, const uint16_t *tilemapregs)
{
    const uint32_t attr = tilemapregs[0], ctrl = tilemapregs[1];
    if (!(ctrl & 0x0008)) return;
    if ((int)((attr & 0x3000) >> 12) != priority) return;
    const uint32_t gfx_addr = (uint32_t)gfx_seg * 0x40;
    if (ctrl & 0x0001) { draw_linemap(p, mem, scanline, gfx_addr, scrollregs, tilemapregs); return; }

    uint32_t logical_scanline = scanline;
    if (ctrl & 0x0040) { /* vertical compression */
        logical_scanline = p->ycmp_table[scanline];
        if (logical_scanline == 0xffffffff) return;
    }

    const uint32_t total_width = 512, y_mask = 0xff, screenwidth = 320;
    const uint32_t drawwidthmask = total_width - 1;
    const uint32_t xscroll = scrollregs[0], yscroll = scrollregs[1];
    const uint32_t tilemap_rambase = tilemapregs[2], exattr_rambase = tilemapregs[3];
    const int tile_width = (int)((attr & 0x0030) >> 4);
    const uint32_t tile_h = 8u << ((attr & 0x00c0) >> 6);
    const uint32_t tile_w = 8u << tile_width;
    const uint32_t tile_count_x = total_width / tile_w;
    const uint32_t bitmap_y = (logical_scanline + yscroll) & y_mask;
    const uint32_t y0 = bitmap_y / tile_h, tile_scanline = bitmap_y % tile_h;
    const uint32_t nc_bpp = ((attr & 3) + 1) << 1;
    const uint32_t bits_per_row = nc_bpp * tile_w / 16;
    const uint8_t blendlevel = blend_levels[mem->vregs[0x2a] & 3];
    const uint32_t words_per_tile = bits_per_row * tile_h;

    int realxscroll = (int)xscroll;
    if (ctrl & 0x0010) realxscroll += (int16_t)mem->scroll[(logical_scanline + yscroll) & 0xff];

    const int upperscrollbits = realxscroll >> (tile_width + 3);
    const int endpos = (int)((screenwidth + tile_w) / tile_w);

    for (uint32_t x0 = 0; (int)x0 < endpos; x0++) {
        const uint32_t realx0 = (x0 + (uint32_t)upperscrollbits) & (tile_count_x - 1);
        const uint32_t tile_address = realx0 + tile_count_x * y0;
        const uint32_t tile = (ctrl & 0x0004) ? RD(tilemap_rambase) : RD(tilemap_rambase + tile_address);
        if (!tile) continue;

        uint32_t tileattr = attr, tilectrl = ctrl;
        if ((tilectrl & 2) == 0) {
            uint16_t ex = (tilectrl & 0x0004) ? RD(exattr_rambase) : RD(exattr_rambase + tile_address / 2);
            if (realx0 & 1) ex >>= 8;
            else ex &= 0x00ff;
            tileattr &= ~0x000cu; tileattr |= (ex >> 2) & 0x000c;   /* flip */
            tileattr &= ~0x0f00u; tileattr |= (ex << 8) & 0x0f00;   /* palette */
            tilectrl &= ~0x0100u; tilectrl |= (ex << 2) & 0x0100;   /* blend */
        }
        uint32_t palette_offset = (tileattr & 0x0f00) >> 4;
        palette_offset >>= nc_bpp;
        palette_offset <<= nc_bpp;
        const int drawx = (int)(x0 * tile_w - ((uint32_t)realxscroll & (tile_w - 1)));
        draw_tilestrip(p, mem, screenwidth, drawwidthmask, (tilectrl & 0x0100) != 0, (tileattr & 4) != 0,
                       tile_h, tile_w, gfx_addr, tile, tile_scanline, drawx, (tileattr & 8) != 0,
                       palette_offset, nc_bpp, bits_per_row, words_per_tile, blendlevel);
    }
}

static void draw_sprite(ppu_t *p, const ppu_mem_t *mem, uint32_t scanline, int priority,
                        uint32_t gfx_addr, uint32_t base)
{
    const uint32_t tile = mem->sprites[base];
    int16_t x = (int16_t)mem->sprites[base + 1];
    int16_t y = (int16_t)mem->sprites[base + 2];
    const uint16_t attr = mem->sprites[base + 3];
    if (!tile) return;
    if ((int)((attr & 0x3000) >> 12) != priority) return;

    const uint32_t screenwidth = 320, screenheight = 256, xmask = 0x1ff, ymask = 0x1ff;
    const uint32_t tile_h = 8u << ((attr & 0x00c0) >> 6);
    const uint32_t tile_w = 8u << ((attr & 0x0030) >> 4);
    if (!(mem->vregs[0x42] & 0x0002)) { /* centred coordinates */
        x = (int16_t)((int)((screenwidth / 2) + x) - (int)(tile_w / 2));
        y = (int16_t)((int)((screenheight / 2) - y) - (int)(tile_h / 2));
    }
    x = (int16_t)(x & xmask);
    y = (int16_t)(y & ymask);
    const int firstline = y;
    const int lastline = (int)((uint32_t)(y + (int)(tile_h - 1)) & ymask);

    const int blend = (attr & 0x4000) != 0, flip_x = (attr & 4) != 0, flip_y = (attr & 8) != 0;
    const uint32_t nc_bpp = ((attr & 3) + 1) << 1;
    const uint32_t bits_per_row = nc_bpp * tile_w / 16;
    const uint8_t blendlevel = blend_levels[mem->vregs[0x2a] & 3];
    const uint32_t words_per_tile = bits_per_row * tile_h;
    uint32_t palette_offset = (attr & 0x0f00) >> 4;
    palette_offset >>= nc_bpp;
    palette_offset <<= nc_bpp;

#define STRIP(scanx) draw_tilestrip(p, mem, screenwidth, xmask, blend, flip_x, tile_h, tile_w, gfx_addr, tile, \
                                    (uint32_t)(scanx), x, flip_y, palette_offset, nc_bpp, bits_per_row, \
                                    words_per_tile, blendlevel)
    if (firstline < lastline) {
        const int scanx = (int)scanline - firstline;
        if (scanx >= 0 && (int)scanline <= lastline) STRIP(scanx);
    } else { /* wraps: clipped from the top, then against the bottom */
        int scanx = (int)scanline - (firstline - (int)(screenheight << 1));
        if (scanx >= 0 && (int)scanline <= lastline) STRIP(scanx);
        scanx = (int)scanline - firstline;
        if (scanx >= 0 && (int)scanline <= lastline + (int)(screenheight << 1)) STRIP(scanx);
    }
#undef STRIP
}

void ppu_render(ppu_t *p, const ppu_mem_t *mem, uint32_t *out)
{
    const uint16_t *v = mem->vregs;
    const int sprlimit = 256; /* SPG24x */
    for (uint32_t scanline = 0; scanline < PPU_H; scanline++) {
        update_palette_lookup(p, v[0x30], v[0x3c]);
        for (int x = 0; x < PPU_W; x++) p->linebuf[x] = 0x8000;
        for (int pri = 0; pri < 4; pri++) {
            draw_page(p, mem, scanline, pri, v[0x20], v + 0x10, v + 0x12);
            draw_page(p, mem, scanline, pri, v[0x21], v + 0x16, v + 0x18);
            if (v[0x42] & 0x0001)
                for (int n = 0; n < sprlimit; n++)
                    draw_sprite(p, mem, scanline, pri, 0x40u * v[0x22], 4u * n);
        }
        uint32_t *row = out + scanline * PPU_W;
        for (int x = 0; x < PPU_W; x++) {
            const uint16_t px = (p->linebuf[x] & 0x8000) ? 0 : p->linebuf[x];
            row[x] = p->lut[px];
        }
    }
}
