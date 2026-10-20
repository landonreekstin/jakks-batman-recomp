/* Sound for the versus mode: the game's own sound driver plays it (vssound.c).
 *
 * The machine keeps running the game in the background (at its title menu, with no buttons),
 * so its driver keeps the music going, and sounds are played by calling the game's own
 * play-sound function from its vblank wait loop. The ids are the game's (versus.h VS_SND_*). */
#ifndef VSSOUND_H
#define VSSOUND_H

#include <stdint.h>

typedef struct spg spg_t;

/* registers the hooks (before mods_attach) */
void vssound_attach(spg_t *m);
/* resets the machine and runs the game to its title menu, quietly (for --versus on its own) */
void vssound_boot(spg_t *m);
/* plays sound (or music) id at the machine's next frame */
void vssound_play(uint16_t id);

#endif
