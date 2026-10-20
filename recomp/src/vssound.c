/* Sound for the versus mode: see vssound.h.
 *
 * 0x00ECB7(id) is the game's play-sound function (the title menu calls it with 0x3F for the
 * cursor, 0x07 for "no" and 0x40 for a choice). It looks id up in a table the game sets up at
 * boot (RAM 0x2544/0x2545 points to it, 0x2550 holds the count: 0x69 at the title and in Street
 * Party alike), so from the title menu it plays any of them, the levels' music included (0x61
 * the title's, 0x62 Street Party's). Bits 0-1 of RAM 0x209E switch sounds and music on.
 *
 * The call: the game waits for vblank in 0x005CDE, spinning at 0x005CE1 (r1 = the old frame
 * counter, r2 = its address). There a hook pushes the id and a return into a trampoline in the
 * upper ROM copy (which the game never runs), as a far call does, and jumps to 0x00ECB7. Its
 * retf lands on the trampoline, whose hook drops the argument, puts r1-r4 back and resumes the
 * wait. */
#include "vssound.h"

#include "mod.h"
#include "spg.h"

#define WAIT_LOOP 0x005CE1
#define PLAY_SOUND 0x00ECB7
#define TRAMP 0x3FFF20
#define TITLE_FRAMES 700     /* the title menu is up (and its music on) by vblank ~600 */
#define QUEUE 32

static uint16_t queue[QUEUE];
static int queued, busy;
static uint16_t keep[4];

void vssound_play(uint16_t id)
{
    if (queued < QUEUE) queue[queued++] = id;
}

static void push(spg_t *m, uint16_t v) { m->ram[m->cpu.r[REG_SP]--] = v; }

static int on_wait(spg_t *m, void *user)
{
    (void)user;
    if (busy || !queued) return 0;
    uint16_t *r = m->cpu.r;
    const uint16_t id = queue[0];
    for (int i = 1; i < queued; i++) queue[i - 1] = queue[i];
    queued--;
    for (int i = 0; i < 4; i++) keep[i] = r[REG_R1 + i];
    push(m, id);
    push(m, TRAMP & 0xFFFF);
    push(m, (uint16_t)((r[REG_SR] & ~0x3f) | (TRAMP >> 16)));
    r[REG_SR] = (uint16_t)(r[REG_SR] & ~0x3f);   /* segment 0 */
    r[REG_PC] = PLAY_SOUND & 0xFFFF;
    m->cpu.cycles += 9;
    busy = 1;
    return 1;
}

static int on_return(spg_t *m, void *user)
{
    (void)user;
    if (!busy) return 0;
    uint16_t *r = m->cpu.r;
    r[REG_SP]++;   /* the argument */
    for (int i = 0; i < 4; i++) r[REG_R1 + i] = keep[i];
    r[REG_SR] = (uint16_t)(r[REG_SR] & ~0x3f);
    r[REG_PC] = WAIT_LOOP & 0xFFFF;
    busy = 0;
    return 1;
}

void vssound_attach(spg_t *m)
{
    (void)m;
    mods_hook(WAIT_LOOP, on_wait, NULL);
    mods_hook(TRAMP, on_return, NULL);
}

void vssound_boot(spg_t *m)
{
    void (*out)(void *, int16_t, int16_t) = m->spu.out;
    const int render = m->render;
    m->spu.out = NULL;
    m->render = 0;
    spg_reset(m);
    mods_reset(m);
    m->buttons = 0;
    while (m->frame < TITLE_FRAMES) spg_run_frame(m, NULL, NULL);
    m->spu.out = out;
    m->render = render;
}
