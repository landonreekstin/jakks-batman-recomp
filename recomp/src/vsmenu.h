/* "Versus" on the title menu (vsmenu.c, notes/versus.md). Native front ends only: it changes the
 * title menu, so a run with it no longer matches MAME there (recordings and netplay leave it off). */
#ifndef VSMENU_H
#define VSMENU_H

#include <stdint.h>

typedef struct spg spg_t;

/* adds the item to the ROM image (after its content id is taken: states stay compatible) */
void vsmenu_patch(uint16_t *rom);
/* registers the hooks (before mods_attach); picking the item sets m->versus_request */
void vsmenu_attach(spg_t *m);

#endif
