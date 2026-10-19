/* Two-player co-op (coop.h, notes/coop.md).
 *
 * The game's object loop (0x07E8A5) calls every object's update through `pop sr, pc` at
 * DISPATCH. A hook there sees each call before it happens:
 *   - the player object: its return goes to TRAMP_P2. When P1's update returns there,
 *     P2's words go into RAM (P1's are kept) and P2's own update function (its object word 6)
 *     is called, returning to TRAMP_DONE, where P1's words come back;
 *   - objects in the pool (enemies, pickups): each has a target, the nearer player. If it is P2,
 *     P2's words are in RAM for that update (back at TRAMP_OBJ_DONE), so its AI, hit tests and
 *     damage see and hit P2.
 * The trampolines are in the upper ROM copy, which the game never runs; only the hooks execute
 * there.
 *
 *   - collisions are a separate pass after the object loop (COLL_PASS): pairs of objects, each
 *     told about the other through its handler (object word 7); the player's applies damage. P2
 *     is not in the game's lists, so after P1's pass P2's words go into RAM and each object is
 *     tested against the player the way 0x0DD0AD tests a pair (on_coll).
 *
 *   - deaths: when P2's health runs out (LOSE_LIFE with P2's words in RAM), P2 loses one of its
 *     own lives and is out of play for P2_RESPAWN frames, then comes back beside P1 with full
 *     health; with none left it stays out. P1's death is the game's own (a checkpoint restart),
 *     and P2 is put back beside P1 when it happens.
 *
 *   - the HUD: the health/lives and special objects run a second time with P2's words (and
 *     lives) in RAM, and the sprites they add are moved to the bottom of the screen.
 *
 * Where things are: levels lay out their objects differently (the player is at 0x017C in Street
 * Party, 0x0130 in the detective levels), so the player and the HUD objects are found through the
 * game's pointers to them (PLAYER_PTR, HUD_H_PTR, HUD_S_PTR) and checked by their update function.
 * Co-op runs only in levels with the brawler HUD; elsewhere (menus, Fast Lane) nothing is touched.
 * An object-loop pass without the player ends P2's level; the next one starts it again.
 *
 * Doors: a door object's collision handler (0x065314) opens on X and, once open, starts the
 * player's walk through it (object word 6 -> DOOR_WALK) on the door's direction, reading the
 * buttons from PAD_HELD/PAD_PRESSED; P2's collision pass puts P2's buttons there. The walk moves the
 * player into the next room. Whoever starts it, P1 walks through: if it is P2, P1 takes over P2's
 * object (keeping its own health and special) and P2 steps out. P2 rejoins on P1's spot, in front
 * of the door, when the walk is over (word 6 back to the normal update).
 *
 * Searching: holding X at a searchable object (word 6 = SEARCHABLE) starts a search (its word
 * SEARCH_ON nonzero) with a progress bar, then a message ([GAME_STATE] = 5) waits for a button.
 * The searcher is the player the object followed when it started (its update reads that player's
 * buttons), until the search is over and the message closed. One search at a time: meanwhile searchable objects are hidden from the other player's
 * collision pass (which also kept cancelling the search) and follow the searcher, but the other
 * player can move and fight. During the message both are frozen and only the searcher's buttons
 * count: if that is P2, the game's own pad words get P2's buttons and P1's update gets none.
 *
 * The camera: the player update calls the camera follow (0x00C695) with the player's position.
 * P1's call gets the midpoint of the two players instead; P2's returns at once. The leash keeps
 * the two at most LEASH pixels apart, so both stay on screen.
 *
 * P2's look: while P2's words are in RAM, sprite-bank reads go to the upper ROM copy (where
 * mods/robin-p2 puts Robin's frames and tiles), and during P2's update Batman's palette id draws
 * with bank 9, which gets P2's colours from the upper copy every frame. Without that mod P2 is a
 * second Batman. */
#include "coop.h"

#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "spg.h"

#define OBJ_LOOP 0x07E8A5      /* the object loop (once a frame) */
#define DISPATCH 0x07E8BA      /* object loop: pop sr, pc from [r1] (the node is on the stack) */
#define POOL_STRIDE 0x4C       /* objects are 76 words; enemies, pickups and effects follow the HUD's */
/* the game's pointers to its objects, each to the object + PTR_OFS */
#define PLAYER_PTR 0x270A
#define HUD_H_PTR 0x2708
#define HUD_S_PTR 0x2705
#define PTR_OFS 0x20
#define HUD_H_FN 0x0DD3D6      /* the brawler health/lives HUD (Fast Lane's is 0x0DD610) */
#define HUD_S_FN 0x07FF39      /* the special meter */
#define OBJ_X 0x15             /* an object's X (as for the player, 0x017C + 0x15 = 0x0191) */
#define TARGET_STICK 32        /* an object changes target only when the other player is this much nearer */
#define CAMERA_FOLLOW 0x00C695 /* camera follow (x, y, ...): centres on x - 160, eased */
#define FAR_READ 0x0DC492      /* reads ds:[[0x26EF] + off] with DS = [0x26EE] */
#define BANK_SEG 0x26EE
#define BANK_OFF 0x26EF
#define SPRITE_BANK_SEG 0x05   /* the sprite bank, 0x051C40 */
#define SPRITE_BANK_OFF 0x1C40
#define P2_BANK_SEG 0x25       /* P2's copy of it, in the upper ROM half (mods/robin-p2) */
#define PAL_TABLE 0x2552       /* palette id -> sprite attribute (palette bank in bits 8-11) */
#define BATMAN_PAL_ID 3
#define P2_ATTR 0x0951         /* P2 draws with palette bank 9, free in the brawler levels */
#define P2_PAL_BANK 9
#define P2_PAL_ROM 0x251870    /* P2's 16 colours: Batman's palette in the upper copy */
#define TRAMP_P2 0x3FFF00
#define TRAMP_DONE 0x3FFF01
#define TRAMP_OBJ_DONE 0x3FFF02
#define TRAMP_COLL 0x3FFF03
#define TRAMP_HUD 0x3FFF04
#define TRAMP_HUD_DONE 0x3FFF05

#define SPRITE_COUNT 0x20A0     /* the sprite list being built: count, then 4 words each from 0x20A3 */
#define SPRITE_LIST 0x20A3
#define P2_HUD_DY (-196)        /* P2's HUD: P1's moved to the bottom of the screen (y is up) */

#define COLL_PASS 0x0DD16D      /* collisions: 0x0DD0AD(A) for every object A in the update list */
#define COLL_PASS_RUN 0x0DD177  /* reached only when the pass isn't skipped */
#define BOX_TEST 0x00B83D      /* (A, B): do their frames' boxes meet */
/* the object words 0x0DD0AD tests a pair (A before B in x) with */
#define OBJ_LEFT 4             /* x of the left edge (the chain through word 0 is sorted by it) */
#define OBJ_Y 0x16
#define OBJ_LO 0x17            /* x extent: [0x19] - [0x17] */
#define OBJ_H 0x18
#define OBJ_HI 0x19
#define OBJ_DEPTH 0x1A
#define OBJ_IS 0x1C            /* collision categories it is, */
#define OBJ_HITS 0x1D          /* and those it reacts to */
#define OBJ_HANDLER 7          /* far pointer to its collision handler (self, other) */

#define PAD_HELD 0x080A    /* buttons held (port A & 0xFFF0), written by 0x0DD77F */
#define PAD_PRESSED 0x080B /* newly pressed this frame */
/* player object words (Street Party addresses in brackets) */
#define POS_X 0x20         /* X, 16.16 (0x019C integer, 0x019D fraction); Y at 0x22 (0x019E) */
#define POS_Y 0x22
#define POS_X_COPY OBJ_X   /* (0x0191) copied from POS_X by the update; what enemies read */
#define POS_X_OLD 0x45     /* (0x01C1) another copy */
#define CTX_FIRST 4        /* P2's words: the object minus its list links */
#define CTX_LAST 0x4B

#define HEALTH 0x21        /* health/lives HUD word (0x01E9): 0..0x20 */
#define HEALTH_FULL 0x20
#define SPECIAL 0x30       /* special HUD words 0x30, 0x31 (0x0244) */
#define LIVES 0x2099
#define GAME_STATE 0x2706  /* 0 playing, 0x0A the player is dying, 4 restarting at a checkpoint */
#define LOSE_LIFE 0x0DD43C /* called when health would go below 0: a life, or Game Over */

#define SEARCHABLE 0x5DA1  /* a searchable object's word 6: {0x07, 0xF37C} */
#define SEARCH_ON 0x2F     /* its word, nonzero while it is being searched */
#define SEARCH_MESSAGE 5   /* GAME_STATE while a message (a search result, a locked door) is shown */
#define GAME_PAUSED 1
#define DOOR_WALK 0x5D80   /* object word 6 while the player walks through a door: {0x06, 0x528A} */

#define P2_RESPAWN 180     /* frames P2 is out after losing a life */
#define LEASH 272          /* the most the players can be apart: one screen (320) minus margins */

/* P2's state: the player object minus its list links, health, special */
#define CTX_N (CTX_LAST - CTX_FIRST + 1)
#define CTX_HEALTH CTX_N
#define CTX_SPECIAL (CTX_N + 1)
#define CTX_WORDS (CTX_N + 3)

static int ctx_words(const coop_t *c, uint16_t *out)
{
    int n = 0;
    for (int i = CTX_FIRST; i <= CTX_LAST; i++) out[n++] = (uint16_t)(c->base + i);
    out[n++] = (uint16_t)(c->hud_h + HEALTH);
    out[n++] = (uint16_t)(c->hud_s + SPECIAL);
    out[n++] = (uint16_t)(c->hud_s + SPECIAL + 1);
    return n;
}

/* a player's object word: P1's are in RAM between updates, P2's in coop_t.p2 */
static uint16_t *word(spg_t *m, int player, int ofs)
{
    coop_t *c = &m->coop;
    return player == 0 ? &m->ram[(uint16_t)(c->base + ofs)] : &c->p2[ofs - CTX_FIRST];
}

static uint16_t *health(spg_t *m, int player)
{
    coop_t *c = &m->coop;
    return player == 0 ? &m->ram[(uint16_t)(c->hud_h + HEALTH)] : &c->p2[CTX_HEALTH];
}

/* the update function an object's word 6 points to */
static uint32_t update_fn(spg_t *m, uint16_t node)
{
    if (node >= 0x2800 - 7) return 0;
    const uint16_t fp = m->ram[node + 6];
    return (uint32_t)(spg_peek(m, fp) & 0x3f) << 16 | spg_peek(m, (uint16_t)(fp + 1));
}

/* is this the player, in a level with the brawler HUD? Finds the HUD objects and the pool too. */
static int find_player(spg_t *m, uint16_t node)
{
    coop_t *c = &m->coop;
    if (node != (uint16_t)(m->ram[PLAYER_PTR] - PTR_OFS)) return 0;
    const uint16_t h = (uint16_t)(m->ram[HUD_H_PTR] - PTR_OFS), s = (uint16_t)(m->ram[HUD_S_PTR] - PTR_OFS);
    if (update_fn(m, h) != HUD_H_FN || update_fn(m, s) != HUD_S_FN) return 0;
    c->base = node;
    c->hud_h = h;
    c->hud_s = s;
    uint16_t last = node > h ? node : h;
    if (s > last) last = s;
    c->pool = (uint16_t)(last + POOL_STRIDE);
    return 1;
}

static void move_x(spg_t *m, int player, int16_t x)
{
    *word(m, player, POS_X) = (uint16_t)x;
    *word(m, player, POS_X_COPY) = (uint16_t)x;
    *word(m, player, POS_X_OLD) = (uint16_t)x;
}

static void push(spg_t *m, uint16_t v)
{
    m->ram[m->cpu.r[REG_SP]--] = v;
}

static void swap_in_p2(spg_t *m)
{
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(c, addrs);
    for (int i = 0; i < n; i++) {
        c->p1[i] = m->ram[addrs[i]];
        m->ram[addrs[i]] = c->p2[i];
    }
}

static void swap_out_p2(spg_t *m)
{
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(c, addrs);
    for (int i = 0; i < n; i++) {
        c->p2[i] = m->ram[addrs[i]];
        m->ram[addrs[i]] = c->p1[i];
    }
    if (m->ram[BANK_SEG] == P2_BANK_SEG) m->ram[BANK_SEG] = SPRITE_BANK_SEG;
}

/* points the call being made (return address at [SP+1], [SP+2]) back to a trampoline */
static void redirect_return(spg_t *m, uint32_t tramp)
{
    coop_t *c = &m->coop;
    const uint16_t sp = m->cpu.r[REG_SP];
    c->ret_sr = m->ram[(uint16_t)(sp + 1)];
    c->ret_pc = m->ram[(uint16_t)(sp + 2)];
    m->ram[(uint16_t)(sp + 1)] = (uint16_t)((c->ret_sr & ~0x3f) | (tramp >> 16));
    m->ram[(uint16_t)(sp + 2)] = tramp & 0xFFFF;
}

static void start_level(spg_t *m)
{
    /* P2 starts as a copy of P1, on its spot (anywhere else could be off the map) */
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(c, addrs);
    for (int i = 0; i < n; i++) c->p2[i] = m->ram[addrs[i]];
    c->active = 1;
    c->prev2 = c->buttons2 & 0xFFF0;
    c->last_x[0] = (int16_t)*word(m, 0, POS_X);
    c->last_x[1] = (int16_t)*word(m, 1, POS_X);
    memset(c->target, 0, sizeof(c->target));
    if (!c->lives_set) { /* the first level: as many as P1 */
        c->p2_lives = m->ram[LIVES];
        c->lives_set = 1;
    }
    c->p2_down = 0;
}

/* which player an object goes after: the nearer one, sticking with its choice unless the
 * other is TARGET_STICK pixels nearer */
static int pick_target(spg_t *m, int slot, uint16_t node)
{
    coop_t *c = &m->coop;
    const int ox = (int16_t)m->ram[node + OBJ_X];
    const int d1 = abs(ox - (int16_t)*word(m, 0, POS_X_COPY));
    const int d2 = abs(ox - (int16_t)*word(m, 1, POS_X_COPY));
    int t = c->target[slot];
    if (t == 0 && d2 + TARGET_STICK < d1) t = 1;
    else if (t == 1 && d1 + TARGET_STICK < d2) t = 0;
    return c->target[slot] = (uint8_t)t;
}

static int p2_in_play(const coop_t *c)
{
    return c->active && !c->p2_down && !c->p2_out && !c->p2_door;
}

/* P2 as a fresh copy of P1 (state, animation) on its spot, with full health */
static void respawn_p2(spg_t *m)
{
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(c, addrs);
    for (int i = 0; i < n; i++) c->p2[i] = m->ram[addrs[i]];
    *health(m, 1) = HEALTH_FULL;
    c->last_x[1] = (int16_t)*word(m, 1, POS_X);
    c->p2_down = 0;
}

/* the searchable objects in the update list (up to max), and whether one is being searched */
static int searchables(spg_t *m, uint16_t *out, int max, int *running)
{
    int k = 0;
    *running = 0;
    uint16_t n = m->ram[(uint16_t)(m->ram[0x26FA] + 2)];
    for (int guard = 0; n && n < 0x2800 - SEARCH_ON && guard < 64; guard++) {
        const uint16_t fp = m->ram[n + 6];
        if (fp == 0) break;
        if (fp == SEARCHABLE) {
            if (m->ram[n + SEARCH_ON]) *running = 1;
            if (k < max) out[k++] = n;
        }
        n = m->ram[n + 2];
    }
    return k;
}

static int search_running(spg_t *m)
{
    uint16_t tmp[1];
    int running;
    searchables(m, tmp, 0, &running);
    return running;
}

/* hides the searchable objects from the collision pass about to run (the non-searcher's) */
static void mask_searchables(spg_t *m)
{
    coop_t *c = &m->coop;
    int running;
    c->n_masked = searchables(m, c->masked, 8, &running);
    for (int i = 0; i < c->n_masked; i++) {
        const uint16_t n = c->masked[i];
        c->masked_is[i] = m->ram[n + OBJ_IS];
        c->masked_hits[i] = m->ram[n + OBJ_HITS];
        m->ram[n + OBJ_IS] = 0;
        m->ram[n + OBJ_HITS] = 0;
    }
}

static void unmask_searchables(spg_t *m)
{
    coop_t *c = &m->coop;
    for (int i = 0; i < c->n_masked; i++) {
        const uint16_t n = c->masked[i];
        if (m->ram[n + 6] != SEARCHABLE) continue; /* removed by the pass */
        m->ram[n + OBJ_IS] = c->masked_is[i];
        m->ram[n + OBJ_HITS] = c->masked_hits[i];
    }
    c->n_masked = 0;
}

/* the game is paused or shows a message: 0x00F12F, which skips the game's collision pass */
static int busy(const spg_t *m)
{
    const uint16_t s = m->ram[GAME_STATE];
    return s == GAME_PAUSED || s == SEARCH_MESSAGE;
}

static int in_message(spg_t *m)
{
    return m->coop.searcher && m->ram[GAME_STATE] == SEARCH_MESSAGE;
}

/* has a search started? It is the player the object follows: its update reads that one's buttons */
static void note_searcher(spg_t *m)
{
    coop_t *c = &m->coop;
    if (c->searcher) return;
    uint16_t n[8];
    int running;
    const int k = searchables(m, n, 8, &running);
    for (int i = 0; i < k; i++) {
        if (!m->ram[n[i] + SEARCH_ON]) continue;
        const int slot = (n[i] - c->pool) / POOL_STRIDE;
        c->searcher = n[i] >= c->pool && !((n[i] - c->pool) % POOL_STRIDE) && slot < COOP_TARGETS
                      && p2_in_play(c) ? c->target[slot] + 1 : 1;
        return;
    }
}

/* a new pass of the object loop: if the last one had no player, the level is over for P2 */
static int on_obj_loop(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || c->phase) return 0;
    if (!c->seen) c->active = 0;
    c->seen = 0;
    note_searcher(m); /* after last frame's object updates */
    if (c->searcher && (!c->active || (!search_running(m) && m->ram[GAME_STATE] != SEARCH_MESSAGE)))
        c->searcher = 0;
    /* P2's search result: the game reads P2's buttons (P1's update gets none, below) */
    const uint16_t held = c->buttons2 & 0xFFF0;
    c->keep[7] = held & (uint16_t)~c->prev_g2; /* P2's presses this frame */
    if (c->searcher == 2 && in_message(m)) {
        m->ram[PAD_HELD] = held;
        m->ram[PAD_PRESSED] = c->keep[7];
    }
    c->prev_g2 = held;
    return 0;
}

/* P2 back after a door: P1's state, as the walk left it in the new room (in front of the door),
 * with P2's health and special */
static void rejoin_p2(spg_t *m)
{
    coop_t *c = &m->coop;
    uint16_t keep[3] = {c->p2[CTX_HEALTH], c->p2[CTX_SPECIAL], c->p2[CTX_SPECIAL + 1]};
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(c, addrs);
    for (int i = 0; i < n; i++) c->p2[i] = m->ram[addrs[i]];
    c->p2[CTX_HEALTH] = keep[0];
    c->p2[CTX_SPECIAL] = keep[1];
    c->p2[CTX_SPECIAL + 1] = keep[2]; /* same spot as P1: in front of the door, as the game puts it */
    c->last_x[0] = (int16_t)*word(m, 0, POS_X);
    c->last_x[1] = (int16_t)*word(m, 1, POS_X);
    c->p2_door = 0;
}

/* puts the player's node back in its place in the chain the collision pass walks (word 0 next,
 * word 1 previous, sorted by word OBJ_LEFT, signed; the ends are sentinels), as 0x07E686 does.
 * P2's update sorts it by P2's x, and the pass then missed what was next to P1 (a door). */
static void resort(spg_t *m, uint16_t n)
{
    uint16_t *w = m->ram;
    const int16_t k = (int16_t)w[n + OBJ_LEFT];
    const uint16_t p = w[n + 1];
    if (!p || p >= 0x2800 - OBJ_LEFT || w[n] >= 0x2800 - OBJ_LEFT) return;
    uint16_t at = p; /* the node to put n after */
    for (int guard = 0; guard < 64 && w[at + 1] && w[at + 1] < 0x2800 - OBJ_LEFT
                        && (int16_t)w[at + OBJ_LEFT] > k; guard++)
        at = w[at + 1];
    if (at == p) { /* not to the left: maybe to the right */
        uint16_t q = w[n];
        for (int guard = 0; guard < 64 && q && w[q] && w[q] < 0x2800 - OBJ_LEFT
                            && (int16_t)w[q + OBJ_LEFT] < k; guard++) {
            at = q;
            q = w[q];
        }
    }
    if (at == p) return;
    const uint16_t next = w[n];
    w[p] = next;
    if (next) w[next + 1] = p;
    const uint16_t after = w[at];
    w[at] = n;
    w[n + 1] = at;
    w[n] = after;
    if (after) w[after + 1] = n;
}

/* P2 started walking through a door (P2's words are swapped out): P1 takes it over */
static void p2_door(spg_t *m)
{
    coop_t *c = &m->coop;
    if (c->p2[6 - CTX_FIRST] != DOOR_WALK) return;
    for (int i = CTX_FIRST; i <= CTX_LAST; i++) m->ram[(uint16_t)(c->base + i)] = c->p2[i - CTX_FIRST];
    c->p2_door = 1;
}

static int on_dispatch(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || c->phase) return 0;
    const uint16_t node = m->ram[(uint16_t)(m->cpu.r[REG_SP] + 3)];
    if (find_player(m, node)) {
        c->seen = 1;
        if (!c->active) start_level(m);
        /* P1 restarted at a checkpoint: P2 comes along */
        if (c->game_state == 4 && m->ram[GAME_STATE] == 0 && !c->p2_out) respawn_p2(m);
        c->game_state = m->ram[GAME_STATE];
        if (c->p2_down && --c->p2_down == 0) respawn_p2(m);
        if (m->ram[(uint16_t)(node + 6)] == DOOR_WALK) {
            if (!c->p2_down && !c->p2_out) c->p2_door = 1;
        } else if (c->p2_door) rejoin_p2(m);
        c->keep[6] = c->searcher == 2 && in_message(m);
        if (c->keep[6]) { /* P1 is frozen */
            c->keep[4] = m->ram[PAD_HELD];
            c->keep[5] = m->ram[PAD_PRESSED];
            m->ram[PAD_HELD] = 0;
            m->ram[PAD_PRESSED] = 0;
        }
        redirect_return(m, TRAMP_P2);
        c->phase = 1;
        return 0;
    }
    if (!c->active) return 0;
    if ((node == c->hud_h || node == c->hud_s) && !c->p2_out) {
        redirect_return(m, TRAMP_HUD);
        c->coll_a = node; /* free while the object loop runs */
        c->phase = 5;
        return 0;
    }
    if (!p2_in_play(c) || node < c->pool || (node - c->pool) % POOL_STRIDE) return 0;
    const int slot = (node - c->pool) / POOL_STRIDE;
    if (slot >= COOP_TARGETS) return 0;
    if (c->searcher && m->ram[node + 6] == SEARCHABLE) c->target[slot] = (uint8_t)(c->searcher - 1);
    else pick_target(m, slot, node);
    if (!c->target[slot]) return 0;
    swap_in_p2(m);
    c->keep[0] = m->ram[PAD_HELD]; /* and P2's buttons (a search reads them) */
    c->keep[1] = m->ram[PAD_PRESSED];
    m->ram[PAD_HELD] = c->buttons2 & 0xFFF0;
    m->ram[PAD_PRESSED] = c->keep[7];
    redirect_return(m, TRAMP_OBJ_DONE);
    c->phase = 3;
    return 0;
}

static int on_obj_done(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 3) return 0;
    swap_out_p2(m);
    m->ram[PAD_HELD] = c->keep[0];
    m->ram[PAD_PRESSED] = c->keep[1];
    m->cpu.r[REG_SR] = c->ret_sr;
    m->cpu.r[REG_PC] = c->ret_pc;
    m->cpu.cycles += 8;
    c->phase = 0;
    return 1;
}

/* the camera follow: P1's call aims at the midpoint, P2's does nothing */
static int on_camera(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || !c->phase) return 0;
    uint16_t *r = m->cpu.r;
    if (c->phase == 2) { /* retf */
        r[REG_SR] = m->ram[++r[REG_SP]];
        r[REG_PC] = m->ram[++r[REG_SP]];
        m->cpu.cycles += 8;
        return 1;
    }
    if (!p2_in_play(c)) return 0;
    const uint16_t sp = r[REG_SP]; /* [sp+1] SR, [sp+2] PC, then the arguments */
    const int x1 = (int16_t)m->ram[(uint16_t)(sp + 3)], y1 = (int16_t)m->ram[(uint16_t)(sp + 4)];
    const int x2 = (int16_t)*word(m, 1, POS_X), y2 = (int16_t)*word(m, 1, POS_Y);
    m->ram[(uint16_t)(sp + 3)] = (uint16_t)((x1 + x2) / 2);
    m->ram[(uint16_t)(sp + 4)] = (uint16_t)((y1 + y2) / 2);
    return 0;
}

/* P2's sprite-bank reads come from the upper ROM copy */
static int on_far_read(spg_t *m, void *user)
{
    (void)user;
    if (m->coop.phase >= 2 && m->ram[BANK_SEG] == SPRITE_BANK_SEG && m->ram[BANK_OFF] == SPRITE_BANK_OFF)
        m->ram[BANK_SEG] = P2_BANK_SEG;
    return 0;
}

static int on_p2(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 1) return 0;
    if (c->keep[6]) {
        m->ram[PAD_HELD] = c->keep[4];
        m->ram[PAD_PRESSED] = c->keep[5];
    }
    if (!p2_in_play(c)) { /* P2 is out: back to the object loop */
        m->cpu.r[REG_SR] = c->ret_sr;
        m->cpu.r[REG_PC] = c->ret_pc;
        c->phase = 0;
        return 1;
    }
    swap_in_p2(m);
    c->keep[2] = m->ram[PAL_TABLE + BATMAN_PAL_ID];
    m->ram[PAL_TABLE + BATMAN_PAL_ID] = P2_ATTR;
    c->keep[0] = m->ram[PAD_HELD];
    c->keep[1] = m->ram[PAD_PRESSED];
    const uint16_t held = c->searcher == 1 && in_message(m) ? 0 : c->buttons2 & 0xFFF0; /* P1's result */
    m->ram[PAD_HELD] = held;
    c->pressed2 = held & (uint16_t)~c->prev2;
    m->ram[PAD_PRESSED] = c->pressed2;
    c->prev2 = held;

    /* call P2's update as the object loop does: push the return (PC, then SR), then load SR
     * and PC from the far pointer in its object word 6 */
    uint16_t *r = m->cpu.r;
    push(m, c->base); /* the argument */
    push(m, TRAMP_DONE & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_DONE >> 16)));
    const uint16_t fp = m->ram[c->base + 6];
    r[REG_SR] = spg_peek(m, fp);
    r[REG_PC] = spg_peek(m, (uint16_t)(fp + 1));
    m->cpu.cycles += 9;
    c->phase = 2;
    return 1;
}

/* keeps the two players within LEASH of each other: whoever moved apart is held back */
static void leash(spg_t *m)
{
    coop_t *c = &m->coop;
    int x[2] = {(int16_t)*word(m, 0, POS_X), (int16_t)*word(m, 1, POS_X)};
    if (abs(x[0] - x[1]) > LEASH) {
        const int dir = x[0] > x[1] ? 1 : -1; /* P1 is right of P2 */
        const int away0 = (x[0] - c->last_x[0]) * dir, away1 = (x[1] - c->last_x[1]) * -dir;
        if (away0 >= away1) x[0] = x[1] + dir * LEASH;
        else x[1] = x[0] - dir * LEASH;
        move_x(m, 0, (int16_t)x[0]);
        move_x(m, 1, (int16_t)x[1]);
    }
    c->last_x[0] = (int16_t)x[0];
    c->last_x[1] = (int16_t)x[1];
}

static int on_done(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 2) return 0;
    m->cpu.r[REG_SP]++; /* drop the argument pushed by on_p2 */
    swap_out_p2(m);
    m->ram[PAD_HELD] = c->keep[0];
    m->ram[PAD_PRESSED] = c->keep[1];
    m->ram[PAL_TABLE + BATMAN_PAL_ID] = c->keep[2];
    p2_door(m);
    resort(m, c->base);
    for (int i = 0; i < 16; i++) /* colour 0 too: transparency is bit 15 of the colour */
        m->vram[0x2B00 - 0x2900 + P2_PAL_BANK * 16 + i] = m->rom[P2_PAL_ROM + i];
    if (p2_in_play(c)) leash(m);
    m->cpu.r[REG_SR] = c->ret_sr;
    m->cpu.r[REG_PC] = c->ret_pc;
    m->cpu.cycles += 8;
    c->phase = 0;
    return 1;
}

/* P2's health ran out (P2's words are in RAM): one of P2's lives instead of the game's */
static int on_lose_life(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase < 2) return 0; /* P1: the game's own */
    if (c->p2_lives > 0) {
        c->p2_lives--;
        c->p2_down = P2_RESPAWN;
    } else c->p2_out = 1;
    uint16_t *r = m->cpu.r; /* retf */
    r[REG_SR] = m->ram[++r[REG_SP]];
    r[REG_PC] = m->ram[++r[REG_SP]];
    m->cpu.cycles += 8;
    return 1;
}

/* ---- P2's HUD --------------------------------------------------------------------------- */

static int on_hud(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 5) return 0;
    uint16_t *r = m->cpu.r;
    c->coll_b = m->ram[SPRITE_COUNT]; /* the sprites from here on are P2's */
    swap_in_p2(m);
    c->keep[3] = m->ram[LIVES];
    m->ram[LIVES] = (uint16_t)c->p2_lives;
    push(m, c->coll_a);
    push(m, TRAMP_HUD_DONE & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_HUD_DONE >> 16)));
    const uint16_t fp = m->ram[c->coll_a + 6];
    r[REG_SR] = spg_peek(m, fp);
    r[REG_PC] = spg_peek(m, (uint16_t)(fp + 1));
    m->cpu.cycles += 9;
    c->phase = 6;
    return 1;
}

static int on_hud_done(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 6) return 0;
    uint16_t *r = m->cpu.r;
    r[REG_SP]++; /* the argument */
    for (int i = c->coll_b; i < m->ram[SPRITE_COUNT] && i < 0x100; i++)
        m->ram[SPRITE_LIST + 4 * i + 2] = (uint16_t)(m->ram[SPRITE_LIST + 4 * i + 2] + P2_HUD_DY);
    m->ram[LIVES] = c->keep[3];
    swap_out_p2(m);
    r[REG_SR] = c->ret_sr;
    r[REG_PC] = c->ret_pc;
    m->cpu.cycles += 8;
    c->phase = 0;
    return 1;
}

/* ---- P2's collision pass ---------------------------------------------------------------- */

static int on_coll_pass(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || !p2_in_play(c) || c->phase) return 0;
    const uint16_t sp = m->cpu.r[REG_SP];
    c->coll_ret_sr = m->ram[(uint16_t)(sp + 1)];
    c->coll_ret_pc = m->ram[(uint16_t)(sp + 2)];
    c->coll_n = -1; /* P1's pass is running */
    if (c->searcher == 2) mask_searchables(m); /* P2's search: P1 can't touch it, or start another */
    m->ram[(uint16_t)(sp + 1)] = (uint16_t)((c->coll_ret_sr & ~0x3f) | (TRAMP_COLL >> 16));
    m->ram[(uint16_t)(sp + 2)] = TRAMP_COLL & 0xFFFF;
    c->coll_ran = 0;
    return 0;
}

static int on_coll_run(spg_t *m, void *user)
{
    (void)user;
    if (m->coop.phase == 0) m->coop.coll_ran = 1;
    return 0;
}

/* still in the object list? (a handler can remove objects) */
static int linked(spg_t *m, uint16_t node)
{
    uint16_t n = m->ram[(uint16_t)(m->ram[0x26FA] + 2)];
    for (int guard = 0; n && n < 0x2800 && guard < 64; guard++) {
        if (spg_peek(m, (uint16_t)(n + 6)) == 0) return 0;
        if (n == node) return 1;
        n = m->ram[n + 2];
    }
    return 0;
}

/* 0x0DD0AD's tests before the box test, for A before B in x */
static int near_pair(spg_t *m, uint16_t a, uint16_t b)
{
    const uint16_t *w = m->ram;
    const int width = (int16_t)(w[a + OBJ_HI] - w[a + OBJ_LO]);
    if ((int16_t)(w[b + OBJ_LEFT] - w[a + OBJ_LEFT]) > width) return 0;
    if (!(w[b + OBJ_IS] & w[a + OBJ_HITS]) && !(w[b + OBJ_HITS] & w[a + OBJ_IS])) return 0;
    const int dy = (int16_t)(w[b + OBJ_Y] + w[b + OBJ_H] - (w[a + OBJ_Y] + w[a + OBJ_H]));
    if (dy > 0) return dy < (int16_t)w[a + OBJ_DEPTH];
    return dy > -(int16_t)w[b + OBJ_DEPTH];
}

/* calls fn(x, y) (a near address, or a far pointer's target when far is set), returning to
 * TRAMP_COLL */
static void coll_call(spg_t *m, uint32_t fn, int far, uint16_t x, uint16_t y)
{
    uint16_t *r = m->cpu.r;
    push(m, y);
    push(m, x);
    push(m, TRAMP_COLL & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_COLL >> 16)));
    if (far) {
        r[REG_SR] = spg_peek(m, (uint16_t)fn);
        r[REG_PC] = spg_peek(m, (uint16_t)(fn + 1));
    } else {
        r[REG_SR] = (uint16_t)((r[REG_SR] & ~0x3f) | (fn >> 16));
        r[REG_PC] = fn & 0xFFFF;
    }
    m->cpu.cycles += 9;
}

/* P2's collisions: for each object X, the pair (X, player) in x order, as 0x0DD0AD does it:
 * the near test, then the box test (step 1), then A's handler if A reacts to B (step 2), then B's
 * if B reacts to A (step 3). Each call returns here. */
static int on_coll(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    uint16_t *r = m->cpu.r;
    if (c->coll_n == -1) { /* P1's pass returned: set up P2's */
        unmask_searchables(m);
        /* the game skips its pass while it is busy, and a handler in P1's pass may have just made
         * it busy: a door opened its message, which P2's touching the same door would replace
         * with its label, leaving the game waiting for a message that is gone */
        if (!c->coll_ran || busy(m)) goto done;
        swap_in_p2(m);
        if (c->searcher == 1) mask_searchables(m);
        c->keep[0] = m->ram[PAD_HELD]; /* handlers read the buttons (doors) */
        c->keep[1] = m->ram[PAD_PRESSED];
        m->ram[PAD_HELD] = c->buttons2 & 0xFFF0;
        m->ram[PAD_PRESSED] = c->pressed2;
        c->phase = 4;
        c->coll_n = 0;
        uint16_t n = m->ram[(uint16_t)(m->ram[0x26FA] + 2)];
        for (int guard = 0; n && n < 0x2800 && guard < 64; guard++) {
            if (spg_peek(m, (uint16_t)(n + 6)) == 0) break;
            if (n != c->base && c->coll_n < (int)(sizeof(c->coll_list) / sizeof(c->coll_list[0])))
                c->coll_list[c->coll_n++] = n;
            n = m->ram[n + 2];
        }
        c->coll_i = 0;
        c->coll_step = 0;
    } else if (c->coll_step) {
        r[REG_SP] += 2; /* the two arguments */
    }
    for (;;) {
        const uint16_t a = c->coll_a, b = c->coll_b;
        const uint16_t *w = m->ram;
        switch (c->coll_step) {
        case 1: /* the box test returned */
            if (r[REG_R1] && (w[a + OBJ_HITS] & w[b + OBJ_IS]) && w[a + OBJ_HANDLER]) {
                c->coll_step = 2;
                coll_call(m, w[a + OBJ_HANDLER], 1, a, b);
                return 1;
            }
            /* fall through */
        case 2:
            if (c->coll_step == 1 && !r[REG_R1]) break;
            if ((w[b + OBJ_HITS] & w[a + OBJ_IS]) && w[b + OBJ_HANDLER]) {
                c->coll_step = 3;
                coll_call(m, w[b + OBJ_HANDLER], 1, b, a);
                return 1;
            }
            break;
        default:
            break;
        }
        c->coll_step = 0;
        if (busy(m)) break; /* a handler in this pass made it busy: the same as above */
        /* the next object */
        uint16_t x = 0;
        while (c->coll_i < c->coll_n) {
            x = c->coll_list[c->coll_i++];
            if (!linked(m, x)) continue;
            const int before = (int16_t)w[x + OBJ_LEFT] <= (int16_t)w[c->base + OBJ_LEFT];
            c->coll_a = before ? x : c->base;
            c->coll_b = before ? c->base : x;
            if (near_pair(m, c->coll_a, c->coll_b)) break;
            x = 0;
        }
        if (!x) break;
        c->coll_step = 1;
        coll_call(m, BOX_TEST, 0, c->coll_a, c->coll_b);
        return 1;
    }
    unmask_searchables(m);
    swap_out_p2(m);
    m->ram[PAD_HELD] = c->keep[0];
    m->ram[PAD_PRESSED] = c->keep[1];
    p2_door(m);
    resort(m, c->base);
done:
    c->phase = 0;
    c->coll_n = 0;
    r[REG_SR] = c->coll_ret_sr;
    r[REG_PC] = c->coll_ret_pc;
    m->cpu.cycles += 8;
    return 1;
}

void coop_attach(spg_t *m)
{
    m->coop.enabled = 1;
    mods_hook(OBJ_LOOP, on_obj_loop, NULL);
    mods_hook(DISPATCH, on_dispatch, NULL);
    mods_hook(CAMERA_FOLLOW, on_camera, NULL);
    mods_hook(FAR_READ, on_far_read, NULL);
    mods_hook(TRAMP_P2, on_p2, NULL);
    mods_hook(TRAMP_DONE, on_done, NULL);
    mods_hook(TRAMP_OBJ_DONE, on_obj_done, NULL);
    mods_hook(LOSE_LIFE, on_lose_life, NULL);
    mods_hook(TRAMP_HUD, on_hud, NULL);
    mods_hook(TRAMP_HUD_DONE, on_hud_done, NULL);
    mods_hook(COLL_PASS, on_coll_pass, NULL);
    mods_hook(COLL_PASS_RUN, on_coll_run, NULL);
    mods_hook(TRAMP_COLL, on_coll, NULL);
}
