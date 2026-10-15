/* Minimal PNG writer (RGB, stored deflate blocks) for frame snapshots. */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(0xffffffffu, hdr + 4, 4);
    c = crc(c, data, len) ^ 0xffffffffu;
    uint8_t t[4];
    be32(t, c);
    fwrite(t, 1, 4, f);
}

int png_write_xrgb(const char *path, const uint32_t *px, int w, int h)
{
    static int crc_ready;
    if (!crc_ready) { crc_init(); crc_ready = 1; }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    const size_t raw_len = (size_t)h * (1 + 3 * (size_t)w);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + 3 * (size_t)w);
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            const uint32_t c = px[y * w + x];
            row[1 + 3 * x] = (uint8_t)(c >> 16);
            row[2 + 3 * x] = (uint8_t)(c >> 8);
            row[3 + 3 * x] = (uint8_t)c;
        }
    }

    /* zlib stream of stored blocks */
    const size_t nblocks = (raw_len + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_len + nblocks * 5 + 4);
    size_t zl = 0;
    z[zl++] = 0x78; z[zl++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        const size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[zl++] = off + n == raw_len;
        z[zl++] = n & 0xff; z[zl++] = n >> 8;
        z[zl++] = ~n & 0xff; z[zl++] = (~n >> 8) & 0xff;
        for (size_t i = 0; i < n; i++) {
            z[zl++] = raw[off + i];
            a = (a + raw[off + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    be32(z + zl, (b << 16) | a);
    zl += 4;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zl);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}
