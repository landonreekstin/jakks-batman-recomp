/* Robin's staff: drawn as two extra 16x16 sprites during melee attack frames.
 *
 * There is no room to add pieces to the frames themselves (the shared sprite bank fills its
 * whole 64K-word window, notes/sprites.md), so the staff is added to sprite RAM each frame:
 * - RAM 0x0184 is the player's current frame index;
 * - that frame's forward attack box (made longer by generate.py, REACH) gives the staff's
 *   height and its tip;
 * - the player's first piece, found in sprite RAM by tile and palette bank, gives the screen
 *   position and the facing (flip x).
 * FUN_0dc5ef copies the game's sprite list into sprite RAM; the hook at its last instruction
 * adds the staff after that copy. The tiles are in the ROM's upper half (STAFF_TILE), which
 * generate.py fills; nothing reads it otherwise, so this works in our runtime only. */
#include <stddef.h>

#include "mod_api.h"

#define BANK          0x051C40
#define FRAME_LIST    0x054B4B
#define NFRAMES       183
#define TILE_BASE     0x400
#define PLAYER_FRAME  0x0184
#define SPRITES       0x2C00   /* 256 x (tile, x, y, attr) */
#define SPRITE_COPY_END 0x0DC604 /* pop bp; retf of FUN_0dc5ef */
#define STAFF_TILE    0xFC00   /* ROM word 0x3F0000: middle; 0xFC01: end with the tip */
#define SLOT0         250      /* sprite slots the staff uses (the game uses the low ones) */
#define PLAYER_BANK   0xE

static const batman_api_t *api;
static int shown;

static int s16(uint16_t v) { return (int16_t)v; }

static void put(spg_t *m, int slot, uint16_t tile, int x, int y, uint16_t attr)
{
    const uint32_t a = SPRITES + 4 * slot;
    api->poke(m, a, tile);
    api->poke(m, a + 1, (uint16_t)x);
    api->poke(m, a + 2, (uint16_t)y);
    api->poke(m, a + 3, attr);
}

static void hide(spg_t *m)
{
    if (!shown) return;
    put(m, SLOT0, 0, 0, 0, 0);
    put(m, SLOT0 + 1, 0, 0, 0, 0);
    shown = 0;
}

static int on_sprite_copy(spg_t *m, void *user)
{
    (void)user;
    const uint16_t f = api->peek(m, PLAYER_FRAME);
    if (f >= NFRAMES) { hide(m); return 0; }
    const uint32_t a = BANK + api->peek(m, FRAME_LIST + f);
    const unsigned n = api->peek(m, a), k = api->peek(m, a + 3);
    int found = 0, y0 = 0, y1 = 0, tip = 0;
    for (uint32_t b = a + 4; b + 5 <= a + 4 + k; b += 5) {
        const unsigned type = api->peek(m, b + 4);
        const int x1 = s16(api->peek(m, b + 2));
        if (type != 1 && type != 2 && x1 > 0) {
            y0 = s16(api->peek(m, b + 1));
            y1 = s16(api->peek(m, b + 3));
            tip = x1;
            found = 1;
            break;
        }
    }
    if (!found || !n) { hide(m); return 0; }

    /* first piece: offset from the frame origin and tile (pieces are (dx, dy, tile index)) */
    const uint32_t p = a + 7 + k;
    const int px = s16(api->peek(m, p)), py = s16(api->peek(m, p + 1));
    const uint16_t ptile = (uint16_t)(api->peek(m, p + 2) + TILE_BASE);
    int slot = -1;
    for (int i = 0; i < SLOT0; i++) {
        const uint32_t s = SPRITES + 4 * i;
        if (api->peek(m, s) == ptile && ((api->peek(m, s + 3) >> 8) & 0xF) == PLAYER_BANK) { slot = i; break; }
    }
    if (slot < 0) { hide(m); return 0; }
    const uint32_t s = SPRITES + 4 * slot;
    const int sx = s16(api->peek(m, s + 1)), sy = s16(api->peek(m, s + 2));
    const uint16_t attr = api->peek(m, s + 3);
    const int left = attr & 0x0004;        /* flip x: facing left */

    /* sprite coordinates are centred with y up; frame coordinates have y down */
    const int yc = (y0 + y1) / 2;
    for (int seg = 0; seg < 2; seg++) {
        const int cx = tip - 8 - 16 * seg; /* centre of this 16 px segment, frame x */
        const int x = left ? sx - (cx - px) : sx + (cx - px);
        const int y = sy - (yc - py);
        put(m, SLOT0 + seg, (uint16_t)(STAFF_TILE + (seg == 0)), x, y, attr);
    }
    shown = 1;
    return 0;
}

int batman_mod_init(const batman_api_t *a, void **user)
{
    (void)user;
    if (a->version != BATMAN_MOD_API_VERSION) return 1;
    api = a;
    api->hook(SPRITE_COPY_END, on_sprite_copy, NULL);
    return 0;
}
