/* See mod.txt. */
#include <stddef.h>

#include "mod_api.h"

static const batman_api_t *api;
static unsigned long calls;

static int do_retf(spg_t *m, void *user)
{
    (void)user;
    calls++;
    api->retf(m);
    return 1;
}

static void report(spg_t *m, void *user)
{
    (void)user;
    if (api->frame(m) % 1000 == 0) api->log("vblank %u: %lu RETFs replaced", api->frame(m), calls);
}

int batman_mod_init(const batman_api_t *a, void **user)
{
    (void)user;
    if (a->version != BATMAN_MOD_API_VERSION) return 1;
    api = a;
    api->hook(0x005ce3, do_retf, NULL);
    api->on_frame(report, NULL);
    return 0;
}
