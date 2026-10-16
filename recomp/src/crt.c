/* CRT display filters: see crt.h.
 *
 * Pipeline (all modes except none), in linear light:
 *   1. per source row: optional composite blur (YIQ, at source resolution), then a
 *      horizontal resample to the output width with sharpened linear interpolation;
 *   2. bloom: the linear source at half resolution, Gaussian-blurred, resampled like the rows;
 *   3. per output row: blend the two nearest source lines with a Gaussian beam profile
 *      whose width grows with brightness (energy-normalised, so flat colours keep their
 *      brightness), then the phosphor mask and bloom;
 *   4. crt mode only: a bilinear warp of the flat image onto a curved, vignetted tube.
 * Everything per-pixel that needs exp/pow comes from tables built in crt_new, and each
 * stage is split over a small pthread pool by rows. */
#include "crt.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ppu.h"

#define LV 64 /* brightness steps of the beam-width table */
#define HW (PPU_W / 2)
#define HH (PPU_H / 2)
#define MAX_THREADS 8

typedef struct {
    float beam_sigma_lo, beam_sigma_hi; /* beam sigma (in source lines) at black and white */
    float mask;                         /* aperture-grille strength 0..1 (0 = none) */
    float bloom;                        /* bloom mix */
    float sharp;                        /* horizontal interpolation sharpness (1 = linear) */
    int composite;                      /* 0 none, 1 chroma only, 2 chroma + luma */
    int curve;
} crt_params_t;

static const crt_params_t PARAMS[CRT_MODES] = {
    [CRT_NONE] = { 0 },
    [CRT_SCANLINES] = { 0.20f, 0.36f, 0.00f, 0.00f, 3.0f, 0, 0 },
    [CRT_APERTURE] = { 0.22f, 0.38f, 0.45f, 0.06f, 3.0f, 0, 0 },
    [CRT_COMPOSITE] = { 0.24f, 0.40f, 0.00f, 0.04f, 1.6f, 2, 0 },
    [CRT_CRT] = { 0.22f, 0.40f, 0.35f, 0.12f, 1.6f, 1, 1 },
};

static const char *NAMES[CRT_MODES] = { "none", "scanlines", "aperture", "composite", "crt" };

typedef void (*job_fn)(struct crt *c, int lo, int hi);

struct crt {
    int scale, w, h;
    float to_lin[256];
    uint8_t to_srgb[4096];
    /* horizontal resample: output column -> left source pixel and weight, per mode */
    int *hx;
    float *hw[CRT_MODES];
    /* bloom resample: output column -> left half-res pixel and weight */
    int *bx;
    float *bw;
    /* beam weights [mode][subrow][line 0/1][brightness] */
    float *beam[CRT_MODES];
    float *lin;      /* 240 x 320 x 3: linear source (after composite) */
    float *half;     /* 120 x 160 x 3: bloom work */
    float *half2;
    float *rowbuf;   /* 240 x w x 3: rows resampled to output width */
    float *bloombuf; /* 240 x w x 3 */
    uint32_t *flat;  /* pre-warp image (crt mode) */
    /* warp: per output pixel the top-left flat index (-1 = outside), fixed-point weights, shade */
    int32_t *warp_idx;
    uint8_t *warp_fx, *warp_fy, *warp_shade;

    /* the frame being filtered */
    int mode;
    const uint32_t *src;
    uint32_t *dst;

    /* worker pool: workers run chunks 1..n of each job, the caller runs chunk 0 */
    int nthreads;
    pthread_t th[MAX_THREADS];
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    job_fn job;
    int job_count, gen, pending, quit;
};

const char *crt_mode_name(int mode) { return mode >= 0 && mode < CRT_MODES ? NAMES[mode] : "?"; }

int crt_mode_parse(const char *name)
{
    for (int i = 0; i < CRT_MODES; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

int crt_width(const crt_t *c) { return c->w; }
int crt_height(const crt_t *c) { return c->h; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- thread pool ---------------------------------------------------------------------- */

static void chunk(const crt_t *c, int part, int *lo, int *hi)
{
    const int parts = c->nthreads + 1;
    *lo = (int)((long)c->job_count * part / parts);
    *hi = (int)((long)c->job_count * (part + 1) / parts);
}

static void *worker(void *arg)
{
    crt_t *c = ((void **)arg)[0];
    const int part = (int)(intptr_t)((void **)arg)[1];
    free(arg);
    int seen = 0;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        while (c->gen == seen && !c->quit) pthread_cond_wait(&c->go, &c->mu);
        if (c->quit) break;
        seen = c->gen;
        pthread_mutex_unlock(&c->mu);
        int lo, hi;
        chunk(c, part, &lo, &hi);
        if (lo < hi) c->job(c, lo, hi);
        pthread_mutex_lock(&c->mu);
        if (--c->pending == 0) pthread_cond_signal(&c->done);
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

static void parallel(crt_t *c, job_fn fn, int count)
{
    if (!c->nthreads) {
        fn(c, 0, count);
        return;
    }
    pthread_mutex_lock(&c->mu);
    c->job = fn;
    c->job_count = count;
    c->pending = c->nthreads;
    c->gen++;
    pthread_cond_broadcast(&c->go);
    pthread_mutex_unlock(&c->mu);
    int lo, hi;
    chunk(c, 0, &lo, &hi);
    if (lo < hi) fn(c, lo, hi);
    pthread_mutex_lock(&c->mu);
    while (c->pending) pthread_cond_wait(&c->done, &c->mu);
    pthread_mutex_unlock(&c->mu);
}

/* ---- setup ---------------------------------------------------------------------------- */

crt_t *crt_new(int scale)
{
    if (scale < 1) scale = 1;
    crt_t *c = calloc(1, sizeof(*c));
    c->scale = scale;
    c->w = PPU_W * scale;
    c->h = PPU_H * scale;
    for (int i = 0; i < 256; i++) c->to_lin[i] = powf(i / 255.0f, 2.2f);
    for (int i = 0; i < 4096; i++) c->to_srgb[i] = (uint8_t)lrintf(powf(i / 4095.0f, 1 / 2.2f) * 255.0f);

    c->hx = malloc(sizeof(int) * c->w);
    c->bx = malloc(sizeof(int) * c->w);
    c->bw = malloc(sizeof(float) * c->w);
    for (int x = 0; x < c->w; x++) {
        const float bxf = (x + 0.5f) / (2 * scale) - 0.5f;
        int b0 = (int)floorf(bxf);
        float f = bxf - b0;
        if (b0 < 0) { b0 = 0; f = 0; }
        if (b0 >= HW - 1) { b0 = HW - 2; f = 1; }
        c->bx[x] = b0;
        c->bw[x] = f;
    }
    for (int m = 0; m < CRT_MODES; m++) {
        if (m == CRT_NONE) continue;
        c->hw[m] = malloc(sizeof(float) * c->w);
        for (int x = 0; x < c->w; x++) {
            const float sx = (x + 0.5f) / scale - 0.5f;
            int x0 = (int)floorf(sx);
            float f = sx - x0;
            f = clampf((f - 0.5f) * PARAMS[m].sharp + 0.5f, 0, 1);
            if (x0 < 0) { x0 = 0; f = 0; }
            if (x0 >= PPU_W - 1) { x0 = PPU_W - 2; f = 1; }
            c->hx[x] = x0;
            c->hw[m][x] = f;
        }

        /* beam table, normalised so that a flat field of brightness v averages to v */
        c->beam[m] = malloc(sizeof(float) * scale * 2 * LV);
        for (int v = 0; v < LV; v++) {
            const float sigma = PARAMS[m].beam_sigma_lo + (PARAMS[m].beam_sigma_hi - PARAMS[m].beam_sigma_lo) * v / (LV - 1);
            float sum = 0;
            for (int s = 0; s < scale; s++) {
                const float sy = (s + 0.5f) / scale - 0.5f; /* offset from line centre, in lines */
                const float d0 = sy < 0 ? -sy : sy;       /* nearest line */
                const float d1 = 1 - d0;                  /* the other neighbour */
                const float w0 = expf(-0.5f * (d0 / sigma) * (d0 / sigma));
                const float w1 = expf(-0.5f * (d1 / sigma) * (d1 / sigma));
                c->beam[m][(s * 2 + 0) * LV + v] = w0;
                c->beam[m][(s * 2 + 1) * LV + v] = w1;
                sum += w0 + w1;
            }
            const float norm = scale / sum;
            for (int s = 0; s < scale; s++) {
                c->beam[m][(s * 2 + 0) * LV + v] *= norm;
                c->beam[m][(s * 2 + 1) * LV + v] *= norm;
            }
        }
    }
    c->lin = malloc(sizeof(float) * 3 * PPU_W * PPU_H);
    c->half = malloc(sizeof(float) * 3 * HW * HH);
    c->half2 = malloc(sizeof(float) * 3 * HW * HH);
    c->rowbuf = malloc(sizeof(float) * 3 * c->w * PPU_H);
    c->bloombuf = malloc(sizeof(float) * 3 * c->w * PPU_H);
    c->flat = malloc(sizeof(uint32_t) * c->w * c->h);

    /* tube geometry: cylindrical bulge, rounded corners, vignette */
    const size_t n = (size_t)c->w * c->h;
    c->warp_idx = malloc(sizeof(int32_t) * n);
    c->warp_fx = malloc(n);
    c->warp_fy = malloc(n);
    c->warp_shade = malloc(n);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            const size_t o = (size_t)y * c->w + x;
            const float nx = (x + 0.5f) / c->w * 2 - 1, ny = (y + 0.5f) / c->h * 2 - 1;
            const float u = nx * (1 + 0.035f * ny * ny) * 0.985f;
            const float v = ny * (1 + 0.055f * nx * nx) * 0.985f;
            const float ax = fabsf(u), ay = fabsf(v);
            /* rounded-rectangle edge, radius r in normalised units, antialiased over ~1.5 px */
            const float r = 0.06f, ex = ax - (1 - r), ey = ay - (1 - r);
            const float qx = ex > 0 ? ex : 0, qy = ey > 0 ? ey : 0;
            const float dist = sqrtf(qx * qx + qy * qy) + fminf(fmaxf(ex, ey), 0) - r; /* <0 inside */
            const float edge = clampf(-dist * c->h / 3.0f, 0, 1);
            if (edge <= 0) { c->warp_idx[o] = -1; c->warp_shade[o] = 0; continue; }
            const float u01 = (u + 1) / 2, v01 = (v + 1) / 2;
            const float vig = powf(clampf(16 * u01 * (1 - u01) * v01 * (1 - v01), 0, 1), 0.18f);
            const float fx = u01 * c->w - 0.5f, fy = v01 * c->h - 0.5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            float wx = fx - ix, wy = fy - iy;
            if (ix < 0) { ix = 0; wx = 0; }
            if (iy < 0) { iy = 0; wy = 0; }
            if (ix > c->w - 2) { ix = c->w - 2; wx = 1; }
            if (iy > c->h - 2) { iy = c->h - 2; wy = 1; }
            c->warp_idx[o] = iy * c->w + ix;
            c->warp_fx[o] = (uint8_t)lrintf(wx * 255);
            c->warp_fy[o] = (uint8_t)lrintf(wy * 255);
            c->warp_shade[o] = (uint8_t)lrintf(vig * edge * 255);
        }

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int want = cpus > 1 ? (int)(cpus - 1) : 0; /* leave a core for the emulation */
    if (want > MAX_THREADS) want = MAX_THREADS;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->go, NULL);
    pthread_cond_init(&c->done, NULL);
    for (int i = 0; i < want; i++) {
        void **arg = malloc(2 * sizeof(void *));
        arg[0] = c;
        arg[1] = (void *)(intptr_t)(i + 1);
        if (pthread_create(&c->th[i], NULL, worker, arg)) { free(arg); break; }
        c->nthreads++;
    }
    return c;
}

void crt_free(crt_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    c->quit = 1;
    pthread_cond_broadcast(&c->go);
    pthread_mutex_unlock(&c->mu);
    for (int i = 0; i < c->nthreads; i++) pthread_join(c->th[i], NULL);
    free(c->hx); free(c->bx); free(c->bw);
    for (int m = 0; m < CRT_MODES; m++) { free(c->hw[m]); free(c->beam[m]); }
    free(c->lin); free(c->half); free(c->half2); free(c->rowbuf); free(c->bloombuf); free(c->flat);
    free(c->warp_idx); free(c->warp_fx); free(c->warp_fy); free(c->warp_shade);
    free(c);
}

/* ---- stages --------------------------------------------------------------------------- */

static void job_nearest(crt_t *c, int lo, int hi)
{
    const int s = c->scale;
    for (int y = lo; y < hi; y++) {
        uint32_t *row = c->dst + (size_t)y * s * c->w;
        const uint32_t *in = c->src + y * PPU_W;
        for (int x = 0; x < PPU_W; x++)
            for (int k = 0; k < s; k++) row[x * s + k] = in[x];
        for (int k = 1; k < s; k++) memcpy(row + (size_t)k * c->w, row, sizeof(uint32_t) * c->w);
    }
}

/* one source row -> linear RGB, with the composite bandwidth limit if asked */
static void load_row(const crt_t *c, const uint32_t *src, int composite, float *o)
{
    if (!composite) {
        for (int x = 0; x < PPU_W; x++) {
            const uint32_t p = src[x];
            o[3 * x] = c->to_lin[(p >> 16) & 0xff];
            o[3 * x + 1] = c->to_lin[(p >> 8) & 0xff];
            o[3 * x + 2] = c->to_lin[p & 0xff];
        }
        return;
    }
    /* YIQ on the gamma-encoded signal, as a composite encoder sees it */
    static const float ky[3] = { 0.15f, 0.7f, 0.15f };
    static const float kc[7] = { 0.03f, 0.11f, 0.22f, 0.28f, 0.22f, 0.11f, 0.03f };
    float yy[PPU_W], ii[PPU_W], qq[PPU_W];
    for (int x = 0; x < PPU_W; x++) {
        const uint32_t p = src[x];
        const float r = ((p >> 16) & 0xff) / 255.0f, g = ((p >> 8) & 0xff) / 255.0f, b = (p & 0xff) / 255.0f;
        yy[x] = 0.299f * r + 0.587f * g + 0.114f * b;
        ii[x] = 0.596f * r - 0.274f * g - 0.322f * b;
        qq[x] = 0.211f * r - 0.523f * g + 0.312f * b;
    }
    const int reach = composite >= 2 ? 3 : 2;
    for (int x = 0; x < PPU_W; x++) {
        float Y = yy[x], I = 0, Q = 0, wsum = 0;
        if (composite >= 2) {
            const int xl = x > 0 ? x - 1 : 0, xr = x < PPU_W - 1 ? x + 1 : PPU_W - 1;
            Y = ky[0] * yy[xl] + ky[1] * yy[x] + ky[2] * yy[xr];
        }
        for (int k = -reach; k <= reach; k++) {
            const int xx = x + k < 0 ? 0 : x + k >= PPU_W ? PPU_W - 1 : x + k;
            const float w = kc[k + 3];
            I += w * ii[xx];
            Q += w * qq[xx];
            wsum += w;
        }
        I /= wsum;
        Q /= wsum;
        const float r = Y + 0.956f * I + 0.621f * Q;
        const float g = Y - 0.272f * I - 0.647f * Q;
        const float b = Y - 1.106f * I + 1.703f * Q;
        o[3 * x] = c->to_lin[(int)(clampf(r, 0, 1) * 255 + 0.5f)];
        o[3 * x + 1] = c->to_lin[(int)(clampf(g, 0, 1) * 255 + 0.5f)];
        o[3 * x + 2] = c->to_lin[(int)(clampf(b, 0, 1) * 255 + 0.5f)];
    }
}

static void hresample(const int *idx, const float *wt, int w, const float *in, float *out)
{
    for (int x = 0; x < w; x++) {
        const float *a = in + 3 * idx[x], f = wt[x];
        out[3 * x] = a[0] + (a[3] - a[0]) * f;
        out[3 * x + 1] = a[1] + (a[4] - a[1]) * f;
        out[3 * x + 2] = a[2] + (a[5] - a[2]) * f;
    }
}

static void job_rows(crt_t *c, int lo, int hi)
{
    const crt_params_t *p = &PARAMS[c->mode];
    for (int y = lo; y < hi; y++) {
        float *l = c->lin + (size_t)y * 3 * PPU_W;
        load_row(c, c->src + y * PPU_W, p->composite, l);
        hresample(c->hx, c->hw[c->mode], c->w, l, c->rowbuf + (size_t)y * 3 * c->w);
    }
}

static const float BK[7] = { 0.06f, 0.13f, 0.20f, 0.22f, 0.20f, 0.13f, 0.06f };

/* half-res downsample + horizontal blur: lin -> half2 -> half */
static void job_bloom_h(crt_t *c, int lo, int hi)
{
    for (int y = lo; y < hi; y++) {
        float *d = c->half2 + (size_t)y * 3 * HW;
        const float *a = c->lin + (size_t)(2 * y) * 3 * PPU_W, *b = a + 3 * PPU_W;
        for (int x = 0; x < HW; x++)
            for (int ch = 0; ch < 3; ch++)
                d[3 * x + ch] = 0.25f * (a[6 * x + ch] + a[6 * x + 3 + ch] + b[6 * x + ch] + b[6 * x + 3 + ch]);
        float *o = c->half + (size_t)y * 3 * HW;
        for (int x = 0; x < HW; x++)
            for (int ch = 0; ch < 3; ch++) {
                float s = 0;
                for (int i = -3; i <= 3; i++) {
                    const int xx = x + i < 0 ? 0 : x + i >= HW ? HW - 1 : x + i;
                    s += BK[i + 3] * d[3 * xx + ch];
                }
                o[3 * x + ch] = s;
            }
    }
}

/* vertical blur: half -> half2 */
static void job_bloom_v(crt_t *c, int lo, int hi)
{
    for (int y = lo; y < hi; y++) {
        float *o = c->half2 + (size_t)y * 3 * HW;
        for (int i = 0; i < 3 * HW; i++) o[i] = 0;
        for (int k = -3; k <= 3; k++) {
            const int yy = y + k < 0 ? 0 : y + k >= HH ? HH - 1 : y + k;
            const float *r = c->half + (size_t)yy * 3 * HW, w = BK[k + 3];
            for (int i = 0; i < 3 * HW; i++) o[i] += w * r[i];
        }
    }
}

/* half2 -> bloombuf: one row per source line, at output width */
static void job_bloom_up(crt_t *c, int lo, int hi)
{
    float tmp[3 * HW];
    for (int y = lo; y < hi; y++) {
        const float fy = (y + 0.5f) / 2 - 0.5f;
        int y0 = (int)floorf(fy);
        float f = fy - y0;
        if (y0 < 0) { y0 = 0; f = 0; }
        if (y0 >= HH - 1) { y0 = HH - 2; f = 1; }
        const float *a = c->half2 + (size_t)y0 * 3 * HW, *b = a + 3 * HW;
        for (int i = 0; i < 3 * HW; i++) tmp[i] = a[i] + (b[i] - a[i]) * f;
        hresample(c->bx, c->bw, c->w, tmp, c->bloombuf + (size_t)y * 3 * c->w);
    }
}

static inline uint32_t pack(const crt_t *c, float r, float g, float b)
{
    const int ri = (int)(clampf(r, 0, 1) * 4095), gi = (int)(clampf(g, 0, 1) * 4095), bi = (int)(clampf(b, 0, 1) * 4095);
    return (uint32_t)c->to_srgb[ri] << 16 | (uint32_t)c->to_srgb[gi] << 8 | c->to_srgb[bi];
}

static void job_beam(crt_t *c, int lo, int hi)
{
    const crt_params_t *p = &PARAMS[c->mode];
    const int s = c->scale, w = c->w;
    /* aperture grille: columns of R, G, B phosphor, energy-compensated */
    const float dim = 1 - p->mask, gain = 3 / (1 + 2 * dim);
    const float mask[3][3] = { { gain, dim * gain, dim * gain }, { dim * gain, gain, dim * gain }, { dim * gain, dim * gain, gain } };
    const float *beam = c->beam[c->mode];
    uint32_t *out = p->curve ? c->flat : c->dst;

    for (int oy = lo; oy < hi; oy++) {
        const int sub = oy % s, line = oy / s;
        /* nearest line is `line`; the other neighbour is above for the top half of the cell */
        const float off = (sub + 0.5f) / s - 0.5f;
        int other = off < 0 ? line - 1 : line + 1;
        if (other < 0) other = 0;
        if (other >= PPU_H) other = PPU_H - 1;
        const float *r0 = c->rowbuf + (size_t)line * 3 * w, *r1 = c->rowbuf + (size_t)other * 3 * w;
        const float *b0 = beam + (sub * 2 + 0) * LV, *b1 = beam + (sub * 2 + 1) * LV;
        const float *bl0 = c->bloombuf + (size_t)line * 3 * w, *bl1 = c->bloombuf + (size_t)other * 3 * w;
        const float bt = off < 0 ? -off : off;
        const float bm0 = p->bloom * (1 - bt), bm1 = p->bloom * bt;
        uint32_t *o = out + (size_t)oy * w;
        for (int x = 0; x < w; x++) {
            float v[3];
            for (int ch = 0; ch < 3; ch++) {
                const float a = r0[3 * x + ch], b = r1[3 * x + ch];
                v[ch] = a * b0[(int)(a * (LV - 1) + 0.5f)] + b * b1[(int)(b * (LV - 1) + 0.5f)];
            }
            if (p->mask > 0) {
                const float *mk = mask[x % 3];
                v[0] *= mk[0]; v[1] *= mk[1]; v[2] *= mk[2];
            }
            if (p->bloom > 0)
                for (int ch = 0; ch < 3; ch++) v[ch] += bm0 * bl0[3 * x + ch] + bm1 * bl1[3 * x + ch];
            o[x] = pack(c, v[0], v[1], v[2]);
        }
    }
}

static void job_warp(crt_t *c, int lo, int hi)
{
    const int w = c->w;
    for (size_t i = (size_t)lo * w; i < (size_t)hi * w; i++) {
        const int32_t idx = c->warp_idx[i];
        if (idx < 0) { c->dst[i] = 0; continue; }
        const uint32_t fx = c->warp_fx[i], fy = c->warp_fy[i], sh = c->warp_shade[i];
        const uint32_t p00 = c->flat[idx], p01 = c->flat[idx + 1], p10 = c->flat[idx + w], p11 = c->flat[idx + w + 1];
        uint32_t px = 0;
        for (int sft = 0; sft <= 16; sft += 8) {
            const uint32_t a = (p00 >> sft) & 0xff, b = (p01 >> sft) & 0xff, cc = (p10 >> sft) & 0xff, d = (p11 >> sft) & 0xff;
            const uint32_t top = a * (255 - fx) + b * fx, bot = cc * (255 - fx) + d * fx;
            const uint32_t v = (top * (255 - fy) + bot * fy) / (255 * 255);
            px |= (v * sh / 255) << sft;
        }
        c->dst[i] = px;
    }
}

void crt_apply(crt_t *c, int mode, const uint32_t *src, uint32_t *dst)
{
    c->src = src;
    c->dst = dst;
    c->mode = mode;
    if (mode <= CRT_NONE || mode >= CRT_MODES) {
        parallel(c, job_nearest, PPU_H);
        return;
    }
    parallel(c, job_rows, PPU_H);
    if (PARAMS[mode].bloom > 0) {
        parallel(c, job_bloom_h, HH);
        parallel(c, job_bloom_v, HH);
        parallel(c, job_bloom_up, PPU_H);
    }
    parallel(c, job_beam, c->h);
    if (PARAMS[mode].curve) parallel(c, job_warp, c->h);
}
