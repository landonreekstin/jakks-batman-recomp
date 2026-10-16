/* CRT display filters for the 320x240 framebuffer (CPU, display only: they read a finished
 * frame and never touch the emulation).
 *
 *   none       integer nearest-neighbour upscale
 *   scanlines  Gaussian beam per source line, wider where brighter
 *   aperture   scanlines + aperture-grille RGB mask
 *   composite  luma/chroma bandwidth blur (these units connect over composite) + scanlines
 *   crt        composite-lite + scanlines + mask + bloom + curved, vignetted tube */
#ifndef CRT_H
#define CRT_H

#include <stdint.h>

enum { CRT_NONE, CRT_SCANLINES, CRT_APERTURE, CRT_COMPOSITE, CRT_CRT, CRT_MODES };

typedef struct crt crt_t;

/* output is (320*scale) x (240*scale) */
crt_t *crt_new(int scale);
void crt_free(crt_t *c);
int crt_width(const crt_t *c);
int crt_height(const crt_t *c);
void crt_apply(crt_t *c, int mode, const uint32_t *src, uint32_t *dst);

const char *crt_mode_name(int mode);
int crt_mode_parse(const char *name); /* -1 if unknown */

#endif
