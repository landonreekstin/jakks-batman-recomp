#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* writes 0x00RRGGBB pixels as an 8-bit RGB PNG; returns 0 on error */
int png_write_xrgb(const char *path, const uint32_t *px, int w, int h);

#endif
