/* Two-player co-op (coop.h, notes/coop.md).
 *
 * The game's object loop (0x07E8A5) calls the player object's update, 0x008BF7, once a frame.
 * Its entry hook points the call's return address at TRAMP_P2. When P1's update returns there,
 * P2's words go into RAM (P1's are kept) and the update is called again, returning to
 * TRAMP_DONE. There P1's words come back and the original return address is used. Both
 * trampoline addresses are in the upper ROM copy, which the game never runs; only the hooks
 * execute there.
 *
 * The camera: the update calls the camera follow (0x00C695) with the player's position.
 * P1's call gets the midpoint of the two players instead; P2's returns at once. The
 * leash keeps the two at most LEASH pixels apart, so both stay on screen.
 *
 * P2's look: while P2's update runs, sprite-bank reads go to the upper ROM copy (where
 * mods/robin-p2 puts Robin's frames and tiles), and Batman's palette id draws with bank 9, which
 * gets P2's colours from the upper copy every frame. Without that mod P2 is a second Batman. */
#include "coop.h"

#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "spg.h"

#define PLAYER_UPDATE 0x008BF7
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

static int on_update(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (!c->enabled || c->phase) return 0; /* phase 2: our own call for P2 */
    if (!c->active || c->level != m->ram[LEVEL_ID]) {
        /* a new level: P2 starts as a copy of P1, a little behind */
        uint16_t addrs[COOP_CTX_MAX];
        const int n = ctx_words(addrs);
        for (int i = 0; i < n; i++) c->p2[i] = m->ram[addrs[i]];
        move_x(m, 1, (int16_t)(m->ram[POS_X] - SPAWN_BEHIND));
        c->active = 1;
        c->level = m->ram[LEVEL_ID];
        c->prev2 = c->buttons2 & 0xFFF0;
        c->last_x[0] = (int16_t)m->ram[POS_X];
        c->last_x[1] = (int16_t)*word(m, 1, POS_X);
    }
    uint16_t *r = m->cpu.r;
    c->ret_sr = m->ram[(uint16_t)(r[REG_SP] + 1)];
    c->ret_pc = m->ram[(uint16_t)(r[REG_SP] + 2)];
    m->ram[(uint16_t)(r[REG_SP] + 1)] = (uint16_t)((c->ret_sr & ~0x3f) | (TRAMP_P2 >> 16));
    m->ram[(uint16_t)(r[REG_SP] + 2)] = TRAMP_P2 & 0xFFFF;
    c->phase = 1;
    return 0;
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
    if (m->coop.phase == 2 && m->ram[BANK_SEG] == SPRITE_BANK_SEG && m->ram[BANK_OFF] == SPRITE_BANK_OFF)
        m->ram[BANK_SEG] = P2_BANK_SEG;
    return 0;
}

static int on_p2(spg_t *m, void *user)
{
    (void)user;
    coop_t *c = &m->coop;
    if (c->phase != 1) return 0;
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(addrs);
    for (int i = 0; i < n; i++) {
        c->p1[i] = m->ram[addrs[i]];
        m->ram[addrs[i]] = c->p2[i];
    }
    c->keep[2] = m->ram[PAL_TABLE + BATMAN_PAL_ID];
    m->ram[PAL_TABLE + BATMAN_PAL_ID] = P2_ATTR;
    c->keep[0] = m->ram[PAD_HELD];
    c->keep[1] = m->ram[PAD_PRESSED];
    const uint16_t held = c->buttons2 & 0xFFF0;
    m->ram[PAD_HELD] = held;
    m->ram[PAD_PRESSED] = held & (uint16_t)~c->prev2;
    c->prev2 = held;

    /* call the update again, as `call` does: push PC, then SR */
    uint16_t *r = m->cpu.r;
    push(m, TRAMP_DONE & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP_DONE >> 16)));
    r[REG_SR] = (uint16_t)((r[REG_SR] & ~0x3f) | (PLAYER_UPDATE >> 16));
    r[REG_PC] = PLAYER_UPDATE & 0xFFFF;
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
    uint16_t addrs[COOP_CTX_MAX];
    const int n = ctx_words(addrs);
    for (int i = 0; i < n; i++) {
        c->p2[i] = m->ram[addrs[i]];
        m->ram[addrs[i]] = c->p1[i];
    }
    m->ram[PAD_HELD] = c->keep[0];
    m->ram[PAD_PRESSED] = c->keep[1];
    m->ram[PAL_TABLE + BATMAN_PAL_ID] = c->keep[2];
    if (m->ram[BANK_SEG] == P2_BANK_SEG) m->ram[BANK_SEG] = SPRITE_BANK_SEG;
    for (int i = 0; i < 16; i++) /* colour 0 too: transparency is bit 15 of the colour */
        m->vram[0x2B00 - 0x2900 + P2_PAL_BANK * 16 + i] = m->rom[P2_PAL_ROM + i];
    leash(m);
    m->cpu.r[REG_SR] = c->ret_sr;
    m->cpu.r[REG_PC] = c->ret_pc;
    m->cpu.cycles += 8;
    c->phase = 0;
    return 1;
}

void coop_attach(spg_t *m)
{
    m->coop.enabled = 1;
    mods_hook(PLAYER_UPDATE, on_update, NULL);
    mods_hook(CAMERA_FOLLOW, on_camera, NULL);
    mods_hook(FAR_READ, on_far_read, NULL);
    mods_hook(TRAMP_P2, on_p2, NULL);
    mods_hook(TRAMP_DONE, on_done, NULL);
}
