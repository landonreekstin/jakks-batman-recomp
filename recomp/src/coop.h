/* Two-player co-op in a one-player game (notes/coop.md).
 *
 * The game has one player object (RAM 0x017C, 76 words, update function 0x008BF7) and keeps a
 * few player globals elsewhere. P2 is a second copy of that state: each frame the player update
 * runs twice, the second time with P2's state and buttons swapped into the same RAM. All of it
 * lives in spg_t, so save states (and rollback) cover it. */
#ifndef COOP_H
#define COOP_H

#include <stdint.h>

#define COOP_CTX_MAX 96
#define COOP_TARGETS 32   /* object pool slots (0x0260 + 76 * n) */

typedef struct coop {
    int enabled;
    uint16_t buttons2;        /* P2's controller, port A layout (bits 8..15) */
    uint16_t prev2;           /* P2's held buttons last time its update ran */
    int active;               /* P2 exists in the current level */
    uint16_t level;           /* level id (RAM 0x015A) P2 was created in */
    int phase;                /* 0 idle, 1 P1's update, 2 P2's, 3 an object's with P2 in, 4 P2's collisions */
    uint16_t ret_sr, ret_pc;  /* where the game's call of the player update returns to */
    uint16_t p1[COOP_CTX_MAX];   /* P1's words while P2's are swapped in */
    uint16_t p2[COOP_CTX_MAX];   /* P2's words while P1's are in RAM */
    uint16_t keep[8];         /* shared words P2's update must not change (camera) */
    uint8_t target[COOP_TARGETS]; /* each pool object's player: 0 P1, 1 P2 */
    int16_t last_x[2];
    /* P2's collision pass (coop.c): the calls of 0x0DD0AD still to make, and the links patched for
       the current one */
    int coll_ran;
    uint16_t coll_ret_sr, coll_ret_pc;
    uint16_t coll_list[COOP_TARGETS + 8];
    int coll_n, coll_i, coll_calling;
    uint16_t coll_a, coll_a0, coll_p0, coll_pred, coll_pred0;        /* both players' X after the previous frame's updates (leash) */
} coop_t;

typedef struct spg spg_t;

/* registers the hooks (before mods_attach) */
void coop_attach(spg_t *m);

#endif
