/* Two-player co-op (coop.h, notes/coop.md).
 *
 * The game's object loop (0x07E8A5) calls every object's update through `pop sr, pc` at
 * DISPATCH. A hook there sees each call before it happens:
 *   - the player object (0x017C): its return goes to TRAMP_P2. When P1's update returns there,
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
 *     is not in the game's lists, so after P1's pass a second one runs with P2's words in RAM,
 *     over the pairs that include the player only (see coll_next).
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "spg.h"

#define DISPATCH 0x07E8BA      /* object loop: pop sr, pc from [r1] (the node is on the stack) */
#define PLAYER_NODE 0x017C
#define POOL_FIRST 0x0260      /* enemies, pickups and effects: 76-word objects from here */
#define POOL_STRIDE 0x4C
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

#define COLL_PASS 0x0DD16D      /* collisions: 0x0DD0AD(A) for every object A in the update list */
#define COLL_PASS_RUN 0x0DD177  /* reached only when the pass isn't skipped */
#define COLL_PAIRS 0x0DD0AD     /* A against the objects after it in the chain through word 0, sorted by word 4 */
#define OBJ_NEXT_X 0           /* that chain */
#define OBJ_LEFT 4             /* its sort key */

#define PAD_HELD 0x080A    /* buttons held (port A & 0xFFF0), written by 0x0DD77F */
#define PAD_PRESSED 0x080B /* newly pressed this frame */
#define LEVEL_ID 0x015A
#define POS_X 0x019C       /* player X, 16.16 (0x019C integer, 0x019D fraction); Y at 0x019E */
#define POS_Y 0x019E
#define POS_X_COPY 0x0191  /* copied from POS_X by the update; what enemies read */
#define POS_X_OLD 0x01C1   /* another copy */

#define SPAWN_BEHIND 40    /* P2 starts this far left of P1 */
#define LEASH 272          /* the most the players can be apart: one screen (320) minus margins */

/* P2's state: the player object minus its list links (0x017C-0x017F), health, special */
static const uint16_t ctx_ranges[][2] = {
    {0x0180, 0x01C7},
    {0x01E9, 0x01E9},
    {0x0244, 0x0245},
};
#define N_RANGES (sizeof(ctx_ranges) / sizeof(ctx_ranges[0]))

static int ctx_words(uint16_t *out)
{
    int n = 0;
    for (size_t r = 0; r < N_RANGES; r++)
        for (uint16_t a = ctx_ranges[r][0]; a <= ctx_ranges[r][1]; a++) out[n++] = a;
    return n;
}

/* where a context word sits in coop_t.p1/p2 */
static int ctx_index(uint16_t addr)
{
    int n = 0;
    for (size_t r = 0; r < N_RANGES; r++) {
        if (addr >= ctx_ranges[r][0] && addr <= ctx_ranges[r][1]) return n + addr - ctx_ranges[r][0];
        n += ctx_ranges[r][1] - ctx_ranges[r][0] + 1;
    }
    return -1;
}

/* a player's words: P1's are in RAM between updates, P2's in coop_t.p2 */
static uint16_t *word(spg_t *m, int player, uint16_t addr)
{
    return player == 0 ? &m->ram[addr] : &m->coop.p2[ctx_index(addr)];
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
    const int n = ctx_words(addrs);
    for (int i = 0; i < n; i++) {
        c->p1[i] = m->ram[addrs[i]];
        m->ram[addrs[i]] = c->p2[i];
    }
}

static void swap_out_p2(spg_t *m)
{
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(addrs);
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
    /* P2 starts as a copy of P1, a little behind */
    coop_t *c = &m->coop;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(addrs);
    for (int i = 0; i < n; i++) c->p2[i] = m->ram[addrs[i]];
    move_x(m, 1, (int16_t)(m->ram[POS_X] - SPAWN_BEHIND));
    c->active = 1;
    c->level = m->ram[LEVEL_ID];
    c->prev2 = c->buttons2 & 0xFFF0;
    c->last_x[0] = (int16_t)m->ram[POS_X];
    c->last_x[1] = (int16_t)*word(m, 1, POS_X);
    memset(c->target, 0, sizeof(c->target));
}

/* which player an object goes after: the nearer one, sticking with its choice unless the
 * other is TARGET_STICK pixels nearer */
static int pick_target(spg_t *m, int slot, uint16_t node)
{
    coop_t *c = &m->coop;
    const int ox = (int16_t)m->ram[node + OBJ_X];
    const int d1 = abs(ox - (int16_t)m->ram[POS_X_COPY]);
    const int d2 = abs(ox - (int16_t)*word(m, 1, POS_X_COPY));
    int t = c->target[slot];
    if (t == 0 && d2 + TARGET_STICK < d1) t = 1;
    else if (t == 1 && d1 + TARGET_STICK < d2) t = 0;
    return c->target[slot] = (uint8_t)t;
}

static int on_dispatch(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || c->phase) return 0;
    const uint16_t node = m->ram[(uint16_t)(m->cpu.r[REG_SP] + 3)];
    if (node == PLAYER_NODE) {
        if (!c->active || c->level != m->ram[LEVEL_ID]) start_level(m);
        redirect_return(m, TRAMP_P2);
        c->phase = 1;
        return 0;
    }
    if (!c->active || node < POOL_FIRST || (node - POOL_FIRST) % POOL_STRIDE) return 0;
    const int slot = (node - POOL_FIRST) / POOL_STRIDE;
    if (slot >= COOP_TARGETS || !pick_target(m, slot, node)) return 0;
    swap_in_p2(m);
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
    swap_in_p2(m);
    c->keep[2] = m->ram[PAL_TABLE + BATMAN_PAL_ID];
    m->ram[PAL_TABLE + BATMAN_PAL_ID] = P2_ATTR;
    c->keep[0] = m->ram[PAD_HELD];
    c->keep[1] = m->ram[PAD_PRESSED];
    const uint16_t held = c->buttons2 & 0xFFF0;
    m->ram[PAD_HELD] = held;
    m->ram[PAD_PRESSED] = held & (uint16_t)~c->prev2;
    c->prev2 = held;

    /* call P2's update as the object loop does: push the return (PC, then SR), then load SR
     * and PC from the far pointer in its object word 6 */
    uint16_t *r = m->cpu.r;
    push(m, PLAYER_NODE); /* the argument */
    push(m, TRAMP_DONE & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_DONE >> 16)));
    const uint16_t fp = m->ram[PLAYER_NODE + 6];
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
    for (int i = 0; i < 16; i++) /* colour 0 too: transparency is bit 15 of the colour */
        m->vram[0x2B00 - 0x2900 + P2_PAL_BANK * 16 + i] = m->rom[P2_PAL_ROM + i];
    leash(m);
    if (getenv("COOP_DEBUG") && m->frame % 60 == 0)
        fprintf(stderr, "%u P1 x %d hp %d | P2 x %d hp %d fn %04x | targets %d\n", m->frame, (int16_t)m->ram[POS_X], m->ram[0x01E9], (int16_t)*word(m, 1, POS_X), *word(m, 1, 0x01E9), *word(m, 1, 0x0182), (int)(c->target[0]+c->target[1]+c->target[2]+c->target[3]+c->target[4]+c->target[5]+c->target[6]+c->target[7]+c->target[8]+c->target[9]+c->target[10]));
    m->cpu.r[REG_SR] = c->ret_sr;
    m->cpu.r[REG_PC] = c->ret_pc;
    m->cpu.cycles += 8;
    c->phase = 0;
    return 1;
}

/* ---- P2's collision pass ---------------------------------------------------------------- */

static int on_coll_pass(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || !c->active || c->phase) return 0;
    const uint16_t sp = m->cpu.r[REG_SP];
    c->coll_ret_sr = m->ram[(uint16_t)(sp + 1)];
    c->coll_ret_pc = m->ram[(uint16_t)(sp + 2)];
    m->ram[(uint16_t)(sp + 1)] = (uint16_t)((c->coll_ret_sr & ~0x3f) | (TRAMP_COLL >> 16));
    m->ram[(uint16_t)(sp + 2)] = TRAMP_COLL & 0xFFFF;
    c->coll_ran = 0;
    c->coll_n = -1; /* P1's pass is running */
    return 0;
}

static int on_coll_run(spg_t *m, void *user)
{
    (void)user;
    if (m->coop.phase == 0) m->coop.coll_ran = 1;
    return 0;
}

/* undoes the link changes made for the call that just returned */
static void coll_unpatch(spg_t *m)
{
    coop_t *c = &m->coop;
    if (c->coll_a != PLAYER_NODE) {
        m->ram[c->coll_a + OBJ_NEXT_X] = c->coll_a0;
        m->ram[PLAYER_NODE + OBJ_NEXT_X] = c->coll_p0;
    } else {
        m->ram[PLAYER_NODE + OBJ_NEXT_X] = c->coll_p0;
        if (c->coll_pred) m->ram[c->coll_pred + OBJ_NEXT_X] = c->coll_pred0;
    }
}

/* The P2 pass makes the calls in coll_list one at a time, each returning here. An object A
 * before P2 (by word 4) is called with its chain cut to just the player; then the player itself,
 * with its chain starting at the first object after P2's position and P1's place in the chain
 * skipped. So every pair with the player is tested once and no other pair again. */
static int on_coll(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    uint16_t *r = m->cpu.r;
    if (c->coll_n == -1) { /* P1's pass returned: set up P2's */
        if (!c->coll_ran) goto done;
        swap_in_p2(m);
        c->phase = 4;
        const int p2x = (int16_t)m->ram[PLAYER_NODE + OBJ_LEFT];
        c->coll_n = 0;
        uint16_t n = m->ram[(uint16_t)(m->ram[0x26FA] + 2)];
        for (int guard = 0; n && n < 0x2800 && guard < 64; guard++) {
            if (spg_peek(m, (uint16_t)(n + 6)) == 0) break;
            if (n != PLAYER_NODE && (int16_t)m->ram[n + OBJ_LEFT] <= p2x &&
                c->coll_n < (int)(sizeof(c->coll_list) / sizeof(c->coll_list[0])) - 1)
                c->coll_list[c->coll_n++] = n;
            n = m->ram[n + 2];
        }
        c->coll_list[c->coll_n++] = PLAYER_NODE;
        c->coll_i = 0;
        c->coll_calling = 0;
    }
    if (c->coll_calling) {
        r[REG_SP]++; /* the argument */
        coll_unpatch(m);
        c->coll_calling = 0;
    }
    if (c->coll_i < c->coll_n) {
        const uint16_t a = c->coll_list[c->coll_i++];
        c->coll_a = a;
        c->coll_p0 = m->ram[PLAYER_NODE + OBJ_NEXT_X];
        if (a != PLAYER_NODE) {
            c->coll_a0 = m->ram[a + OBJ_NEXT_X];
            m->ram[a + OBJ_NEXT_X] = PLAYER_NODE;
            m->ram[PLAYER_NODE + OBJ_NEXT_X] = 0;
        } else {
            /* the first object at or after P2's position, P1's place skipped */
            const int p2x = (int16_t)m->ram[PLAYER_NODE + OBJ_LEFT];
            uint16_t first = 0;
            int best = 0x7FFF;
            c->coll_pred = 0;
            uint16_t n = m->ram[(uint16_t)(m->ram[0x26FA] + 2)];
            for (int guard = 0; n && n < 0x2800 && guard < 64; guard++) {
                if (spg_peek(m, (uint16_t)(n + 6)) == 0) break;
                if (n != PLAYER_NODE) {
                    const int x = (int16_t)m->ram[n + OBJ_LEFT];
                    if (x > p2x && x < best) { best = x; first = n; }
                    if (m->ram[n + OBJ_NEXT_X] == PLAYER_NODE) c->coll_pred = n;
                }
                n = m->ram[n + 2];
            }
            if (c->coll_pred) {
                c->coll_pred0 = m->ram[c->coll_pred + OBJ_NEXT_X];
                m->ram[c->coll_pred + OBJ_NEXT_X] = c->coll_p0;
            }
            m->ram[PLAYER_NODE + OBJ_NEXT_X] = first;
        }
        push(m, a);
        push(m, TRAMP_COLL & 0xFFFF);
        push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_COLL >> 16)));
        r[REG_SR] = (uint16_t)((r[REG_SR] & ~0x3f) | (COLL_PAIRS >> 16));
        r[REG_PC] = COLL_PAIRS & 0xFFFF;
        m->cpu.cycles += 9;
        c->coll_calling = 1;
        return 1;
    }
    swap_out_p2(m);
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
    mods_hook(DISPATCH, on_dispatch, NULL);
    mods_hook(CAMERA_FOLLOW, on_camera, NULL);
    mods_hook(FAR_READ, on_far_read, NULL);
    mods_hook(TRAMP_P2, on_p2, NULL);
    mods_hook(TRAMP_DONE, on_done, NULL);
    mods_hook(TRAMP_OBJ_DONE, on_obj_done, NULL);
    mods_hook(COLL_PASS, on_coll_pass, NULL);
    mods_hook(COLL_PASS_RUN, on_coll_run, NULL);
    mods_hook(TRAMP_COLL, on_coll, NULL);
}
