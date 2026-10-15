/* SPG24x machine for jak_batm. See spg.h.
 *
 * Timing reproduces MAME's scheduler so traces line up exactly:
 *   - CPU time is cycles * ASPC attoseconds (27 MHz).
 *   - The screen is 320x262 at 60 Hz with the visible area ending at line 239. MAME starts
 *     the machine at the beginning of VBLANK, so vblank_begin k happens at k * FRAME_AS
 *     (k >= 1) and vblank_end at k * FRAME_AS + 22 lines.
 *   - The SPU only advances when MAME updates its sound stream: every 1/50 s, and before
 *     wave-in writes. An update at time t renders samples up to floor(t * 70312) inclusive.
 *   - A timer at time T is seen at the first instruction boundary where
 *     cycles > floor(T / ASPC) (the CPU overshoots its timeslice), and an IRQ it raises is
 *     taken after the next instruction.
 */
#include "spg.h"

#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

#define ASPC      37037037037ULL            /* HZ_TO_ATTOSECONDS(27000000) */
#define FRAME_AS  16666666666666666ULL      /* HZ_TO_ATTOSECONDS(60) */
#define SCREEN_H  262
#define SCREEN_W  320
#define VIS_H     240
#define SCAN_AS   (FRAME_AS / SCREEN_H)
#define PIXEL_AS  (FRAME_AS / (SCREEN_H * SCREEN_W))
#define VBLANK_AS (SCAN_AS * (SCREEN_H - VIS_H))
#define SOUND_UPDATE_AS 20000000000000000ULL /* STREAMS_UPDATE_FREQUENCY = 50 Hz */
#define AS_PER_SEC 1000000000000000000ULL

/* PPU registers (offsets from 0x2800) */
#define V_IRQ_ENABLE 0x62
#define V_IRQ_STATUS 0x63

/* I/O registers (offsets from 0x3d00) */
enum {
    IO_MODE = 0x00,
    IOA_DATA = 0x01, IOA_BUFFER, IOA_DIR, IOA_ATTRIB, IOA_MASK,
    IOB_DATA = 0x06, IOB_BUFFER, IOB_DIR, IOB_ATTRIB, IOB_MASK,
    IOC_DATA = 0x0b, IOC_BUFFER, IOC_DIR, IOC_ATTRIB, IOC_MASK,
    IO_VERT_LINE = 0x1c,
    IO_INT_CTRL = 0x21, IO_INT_CLEAR = 0x22,
    IO_ADC_CTRL = 0x25,
    IO_NTSC_PAL = 0x2b, IO_PRNG1 = 0x2c, IO_PRNG2 = 0x2d,
    IO_DATA_SEGMENT = 0x2f,
};

static int64_t event_threshold(u128 t_as) { return (int64_t)(t_as / ASPC); }

static u128 cpu_time_as(const spg_t *m) { return (u128)m->cpu.cycles * ASPC; }

static int vpos(const spg_t *m)
{
    u128 delta = cpu_time_as(m) - m->vblank_start_as + PIXEL_AS / 2;
    return (int)((VIS_H + (uint64_t)(delta / SCAN_AS)) % SCREEN_H);
}

static uint16_t rom_read(const spg_t *m, uint32_t addr) { return m->rom[addr & 0x1fffff]; }

/* ---- SPU ---------------------------------------------------------------------------- */

static uint16_t bus_read(void *ctx, uint32_t addr);

static uint16_t spu_space_read(void *ctx, uint32_t addr)
{
    spg_t *m = ctx;
    const uint32_t a = addr & 0x3fffff;
    if (a < 0x2800) return m->ram[a];
    if (a >= 0x2900 && a < 0x3000) return m->vram[a - 0x2900];
    if (a >= 0x2800 && a < 0x3e04 && !(a >= 0x3800 && a < 0x3d00))
        fprintf(stderr, "spu: sample fetch from I/O %04x not modelled\n", a);
    return rom_read(m, a);
}

/* sound_stream::update_nodeps at time t: render up to and including sample floor(t * rate) */
static void sound_sync(spg_t *m, u128 t_as)
{
    if (t_as == 0) return;
    const uint64_t idx = (uint64_t)(t_as / AS_PER_SEC) * SPU_RATE +
                         (uint64_t)((t_as % AS_PER_SEC) * SPU_RATE / AS_PER_SEC);
    if (idx + 1 > m->spu.samples_done) spu_generate(&m->spu, (uint32_t)(idx + 1 - m->spu.samples_done));
}

/* ---- PPU ---------------------------------------------------------------------------- */

static void video_check_irq(spg_t *m)
{
    /* fiq_vector stays 0xff (FIQ_SEL never written), so the video IRQ goes to IRQ0 */
    unsp_set_line(&m->cpu, UNSP_IRQ0_LINE, (m->vregs[V_IRQ_STATUS] & m->vregs[V_IRQ_ENABLE]) != 0);
}

static uint16_t video_r(spg_t *m, uint32_t off)
{
    if (off == 0x38) return (uint16_t)vpos(m);
    if (off == 0x3e || off == 0x3f) return 0; /* light gun, not connected */
    return m->vregs[off];
}

static void sprite_dma(spg_t *m, uint32_t len)
{
    const uint32_t src = m->vregs[0x70] & 0x3fff, dst = m->vregs[0x71];
    for (uint32_t j = 0; j < len; j++)
        if (dst + j < 0x400) m->vram[0x300 + dst + j] = bus_read(m, src + j);
    m->vregs[0x72] = 0;
    if (m->vregs[V_IRQ_ENABLE] & 4) {
        m->vregs[V_IRQ_STATUS] |= 4;
        video_check_irq(m);
    }
}

static void video_w(spg_t *m, uint32_t off, uint16_t data)
{
    switch (off) {
    case 0x10: case 0x16: m->vregs[off] = data & 0x01ff; break; /* X scroll */
    case 0x11: case 0x17: m->vregs[off] = data & 0x00ff; break; /* Y scroll */
    case 0x2a: m->vregs[off] = data & 0x0003; break;
    case 0x30: m->vregs[off] = data & 0x00ff; break;
    case 0x36: case 0x37:
        m->vregs[off] = data & 0x01ff;
        if (m->vregs[0x37] < 160 && m->vregs[0x36] < 240)
            fprintf(stderr, "spg: screen-position IRQ armed (%d,%d), not modelled\n", m->vregs[0x36], m->vregs[0x37]);
        break;
    case 0x39: m->vregs[off] = data & 0x0001; break;
    case 0x3d: m->vregs[off] = data & 0x000f; break;
    case 0x3e: case 0x3f: break;
    case V_IRQ_ENABLE: m->vregs[off] = data & 0x0007; video_check_irq(m); break;
    case V_IRQ_STATUS: m->vregs[off] &= ~data; video_check_irq(m); break;
    case 0x70: m->vregs[off] = data & 0x3fff; break;
    case 0x71: m->vregs[off] = data & 0x03ff; break;
    case 0x72: { uint16_t len = data & 0x3ff; sprite_dma(m, len ? len : 0x400); break; }
    default: m->vregs[off] = data; break;
    }
}

/* ---- I/O ---------------------------------------------------------------------------- */

static uint16_t port_in(spg_t *m, int port)
{
    switch (port) {
    case 0: return m->buttons;
    case 2: return (uint16_t)eeprom_read_sda(&m->eeprom); /* "P3": only SDA is connected */
    default: return 0;
    }
}

static void do_gpio(spg_t *m, uint32_t offset, int write)
{
    const uint32_t i = (offset - 1) / 5;
    const uint16_t buffer = m->ioregs[5 * i + 2], dir = m->ioregs[5 * i + 3];
    const uint16_t attr = m->ioregs[5 * i + 4], special = m->ioregs[5 * i + 5];
    const uint16_t push = dir, pull = (uint16_t)~dir;
    uint16_t what = buffer & (push | pull);
    what ^= dir & ~attr;
    what &= ~special;
    if (write && i == 2) { /* jakks_state::portc_w */
        const uint16_t mask = push & ~special;
        if (mask & 2) eeprom_write_scl(&m->eeprom, (what >> 1) & 1);
        if (mask & 1) eeprom_write_sda(&m->eeprom, what & 1);
    }
    what &= ~pull;
    if (!write) what |= port_in(m, (int)i) & pull;
    m->ioregs[5 * i + 1] = what;
}

static uint16_t io_r(spg_t *m, uint32_t off)
{
    switch (off) {
    case IOA_DATA: case IOB_DATA: case IOC_DATA:
        do_gpio(m, off, 0);
        return m->ioregs[off];
    case IO_VERT_LINE: return (uint16_t)vpos(m);
    case IO_NTSC_PAL: return 0; /* NTSC */
    case IO_DATA_SEGMENT: return (m->cpu.r[REG_SR] >> 10) & 0x3f;
    default: return m->ioregs[off];
    }
}

static void io_w(spg_t *m, uint32_t off, uint16_t data)
{
    switch (off) {
    case IOA_DATA: case IOB_DATA: case IOC_DATA:
        off++; /* data writes go to the buffer register */
        /* fall through */
    case IOA_BUFFER: case IOA_ATTRIB: case IOA_DIR:
    case IOB_BUFFER: case IOB_ATTRIB: case IOB_DIR:
    case IOC_BUFFER: case IOC_DIR: case IOC_ATTRIB: case IOC_MASK:
        m->ioregs[off] = data;
        do_gpio(m, off, 1);
        break;
    case IO_INT_CLEAR:
        /* no I/O IRQ is ever enabled by this game (0x3d21 stays 0) */
        m->ioregs[off] &= ~data;
        break;
    case 0x24: break; /* watchdog clear */
    case IO_ADC_CTRL:
        m->ioregs[off] = data & ~0x2000;
        if (data & 1) m->ioregs[off] |= 0x2000; /* conversions are not modelled */
        break;
    case IO_PRNG1: case IO_PRNG2: m->ioregs[off] = data & 0x7fff; break;
    case IO_DATA_SEGMENT:
        m->cpu.r[REG_SR] = (m->cpu.r[REG_SR] & 0x03ff) | (uint16_t)((data & 0x3f) << 10);
        break;
    default: m->ioregs[off] = data; break;
    }
}

/* ---- bus ---------------------------------------------------------------------------- */

static int replayed(uint32_t a)
{
    return (a >= 0x2800 && a < 0x2900) || (a >= 0x3000 && a < 0x3800) || (a >= 0x3d00 && a < 0x3e04);
}

static uint16_t periph_read(spg_t *m, uint32_t a)
{
    if (a < 0x2900) return video_r(m, a - 0x2800);
    if (a < 0x3800) return spu_read(&m->spu, a - 0x3000);
    if (a < 0x3e00) return io_r(m, a - 0x3d00);
    return m->dmaregs[a - 0x3e00];
}

static uint16_t bus_read(void *ctx, uint32_t addr)
{
    spg_t *m = ctx;
    const uint32_t a = addr & 0x3fffff;
    if (a < 0x2800) return m->ram[a];
    if (a >= 0x2900 && a < 0x3000) return m->vram[a - 0x2900];
    if (!replayed(a)) return rom_read(m, a); /* ROM, including the holes in the I/O page */

    uint16_t v = periph_read(m, a);
    if (m->replay && !m->replay_diverged) {
        if (m->replay_pos >= m->replay_len || m->replay[m->replay_pos].addr != a) {
            fprintf(stderr, "replay: diverged at frame %u, pc %06x: read %04x, expected %04x (record %zu)\n",
                    m->frame, unsp_lpc(&m->cpu), a,
                    m->replay_pos < m->replay_len ? m->replay[m->replay_pos].addr : 0, m->replay_pos);
            m->replay_diverged = 1;
        } else {
            const uint16_t rec = m->replay[m->replay_pos++].data;
            if (rec != v) m->model_mismatch[a - 0x2800]++;
            v = rec;
        }
    }
    if (m->iolog) fprintf(m->iolog, "%u R %04X %04X\n", m->frame + 1, a, v);
    return v;
}

static void bus_write(void *ctx, uint32_t addr, uint16_t data)
{
    spg_t *m = ctx;
    const uint32_t a = addr & 0x3fffff;
    if (a < 0x2800) { m->ram[a] = data; return; }
    if (a >= 0x2900 && a < 0x3000) { m->vram[a - 0x2900] = data; return; }
    if (!replayed(a)) return; /* ROM */
    if (m->iolog) fprintf(m->iolog, "%u W %04X %04X\n", m->frame + 1, a, data);
    if (a < 0x2900) video_w(m, a - 0x2800, data);
    else if (a < 0x3800) {
        if (a == 0x3410 || a == 0x3411) sound_sync(m, cpu_time_as(m)); /* wave in: MAME updates the stream */
        spu_write(&m->spu, a - 0x3000, data);
    }
    else if (a < 0x3e00) io_w(m, a - 0x3d00, data);
    else m->dmaregs[a - 0x3e00] = data;
}

/* ---- machine ------------------------------------------------------------------------ */

int spg_init(spg_t *m, const uint16_t *rom)
{
    memset(m, 0, sizeof(*m));
    m->rom = rom;
    m->cpu.ctx = m;
    m->cpu.read = bus_read;
    m->cpu.write = bus_write;
    m->spu.ctx = m;
    m->spu.read = spu_space_read;
    eeprom_init(&m->eeprom);
    return 1;
}

void spg_reset(spg_t *m)
{
    memset(m->ram, 0, sizeof(m->ram));
    memset(m->vregs, 0, sizeof(m->vregs));
    m->vregs[0x36] = m->vregs[0x37] = 0xffff;
    m->vregs[0x3c] = 0x0020; /* renderer reset values */
    m->vregs[0x42] = 0x0001;
    memset(m->vram, 0, sizeof(m->vram));
    spu_reset(&m->spu);
    memset(m->ioregs, 0, sizeof(m->ioregs));
    m->ioregs[0x23] = 0x0028; /* external memory control */
    m->ioregs[IO_PRNG1] = 0x1418;
    m->ioregs[IO_PRNG2] = 0x1658;
    unsp_reset(&m->cpu);
    m->frame = 0;
    m->vblank_start_as = 0;
    m->vbl_end_count = 1;
    m->next_vbl_begin = event_threshold((u128)FRAME_AS);
    m->next_vbl_end = event_threshold((u128)FRAME_AS + VBLANK_AS);
    m->snd_update_count = 1;
    m->next_snd_update = event_threshold((u128)SOUND_UPDATE_AS);
}

void spg_trace_line(spg_t *m, FILE *f)
{
    const uint16_t *r = m->cpu.r;
    fprintf(f, "R1=%04X R2=%04X R3=%04X R4=%04X BP=%04X SP=%04X SR=%04X %06X\n",
            r[REG_R1], r[REG_R2], r[REG_R3], r[REG_R4], r[REG_BP], r[REG_SP], r[REG_SR], unsp_lpc(&m->cpu));
}

int spg_run_frame(spg_t *m, FILE *trace, uint64_t *trace_budget)
{
    for (;;) {
        if (m->cpu.cycles > m->next_snd_update) {
            sound_sync(m, (u128)m->snd_update_count * SOUND_UPDATE_AS);
            m->snd_update_count++;
            m->next_snd_update = event_threshold((u128)m->snd_update_count * SOUND_UPDATE_AS);
        }
        if (m->cpu.cycles > m->next_vbl_end) {
            m->vregs[V_IRQ_STATUS] &= ~1;
            video_check_irq(m);
            m->vbl_end_count++;
            m->next_vbl_end = event_threshold((u128)m->vbl_end_count * FRAME_AS + VBLANK_AS);
        }
        if (m->cpu.cycles > m->next_vbl_begin) {
            m->frame++;
            m->vblank_start_as = (u128)m->frame * FRAME_AS;
            m->next_vbl_begin = event_threshold((u128)(m->frame + 1) * FRAME_AS);
            if (m->on_frame) m->on_frame(m); /* MAME's frame_update runs before the vblank callbacks */
            if (m->vregs[V_IRQ_ENABLE] & 1) {
                m->vregs[V_IRQ_STATUS] |= 1;
                video_check_irq(m);
            }
            return 1;
        }
        if (trace && *trace_budget) {
            spg_trace_line(m, trace);
            (*trace_budget)--;
        }
        unsp_step(&m->cpu);
        if (unsp_bad_opcode) return 0;
    }
}

int spg_load_replay(spg_t *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t cap = 1 << 16;
    m->replay = malloc(cap * sizeof(io_rec_t));
    m->replay_len = 0;
    unsigned fr, a, d;
    char rw;
    while (fscanf(f, "%u %c %x %x", &fr, &rw, &a, &d) == 4) {
        if (rw != 'R') continue;
        if (m->replay_len == cap) m->replay = realloc(m->replay, (cap *= 2) * sizeof(io_rec_t));
        m->replay[m->replay_len++] = (io_rec_t){ fr, (uint16_t)a, (uint16_t)d, rw };
    }
    fclose(f);
    return 1;
}
