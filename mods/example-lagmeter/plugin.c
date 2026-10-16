/* Lag meter: the game paces itself with wait_vblank (0x005CDE), a far function that spins
 * until the IRQ handler bumps the frame counter at [0x0809]. A vblank with no call to it
 * means the game loop needed more than one frame: a lag frame. */
#include <stdio.h>

#include "mod_api.h"

#define WAIT_VBLANK 0x005cde

static const batman_api_t *api;
static unsigned waits, lag, frames;

static int on_wait(spg_t *m, void *user)
{
    (void)m; (void)user;
    waits++;
    return 0; /* run the original */
}

static void on_frame(spg_t *m, void *user)
{
    (void)user;
    if (frames++ && !waits) lag++;
    waits = 0;
    char s[32];
    snprintf(s, sizeof(s), "lag %u", lag);
    api->text(m, 320 - 8 - 6 * 9, 4, s);
}

int batman_mod_init(const batman_api_t *a, void **user)
{
    (void)user;
    if (a->version != BATMAN_MOD_API_VERSION) return 1;
    api = a;
    api->hook(WAIT_VBLANK, on_wait, NULL);
    api->on_frame(on_frame, NULL);
    return 0;
}
