/* Two-player co-op in a one-player game (notes/coop.md).
 *
 * The game has one player object (76 words, update function 0x008BF7; where it sits in RAM depends
 * on the level) and keeps a few player globals elsewhere. P2 is a second copy of that state: each
 * frame the player update runs twice, the second time with P2's state and buttons swapped into the
 * same RAM. All of it lives in spg_t, so save states (and rollback) cover it. */
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
    int seen;                 /* the player's update ran in this pass of the object loop */
    int lives_set;            /* p2_lives has been given its starting value */
    /* where this level put things (coop.c, found through the game's own pointers) */
    uint16_t base;            /* the player object: 0x017C in Street Party, 0x0130 in the detective levels */
    uint16_t hud_h, hud_s;    /* the health/lives and special HUD objects */
    uint16_t pool;            /* the first object after them (enemies, pickups) */
    int phase;                /* 0 idle, 1 P1's update, 2 P2's, 3 an object's with P2 in, 4 P2's collisions */
    uint16_t ret_sr, ret_pc;  /* where the game's call of the player update returns to */
    uint16_t p1[COOP_CTX_MAX];   /* P1's words while P2's are swapped in */
    uint16_t p2[COOP_CTX_MAX];   /* P2's words while P1's are in RAM */
    uint16_t keep[8];         /* shared words P2's update must not change (camera) */
    int p2_lives;             /* P2's own lives (P1's are the game's, RAM 0x2099) */
    int p2_down;              /* frames until P2 comes back (0: in play) */
    int p2_out;               /* P2 has no lives left */
    uint16_t game_state;      /* RAM 0x2706 last frame (0x0A: P1 dying, 4: restarting) */
    uint8_t target[COOP_TARGETS]; /* each pool object's player: 0 P1, 1 P2 */
    int16_t last_x[2];
    /* P2's collision pass (coop.c): the objects to test against the player, and where it is */
    int coll_ran;
    uint16_t coll_ret_sr, coll_ret_pc;
    uint16_t coll_list[COOP_TARGETS + 8];
    int coll_n, coll_i, coll_step;
    uint16_t coll_a, coll_b;
} coop_t;

typedef struct spg spg_t;

/* registers the hooks (before mods_attach) */
void coop_attach(spg_t *m);

#endif
