/* On-screen messages drawn into an XRGB8888 buffer with a built-in 5x7 font. */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>

/* draws text at (x, y) with each font pixel `scale` pixels wide, on a translucent box */
void osd_text(uint32_t *buf, int w, int h, int x, int y, int scale, const char *text);

#endif
