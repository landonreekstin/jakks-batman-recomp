/* Headless runner for the jak_batm reconstruction.
 *
 *   batman-rt <batman.bin> [--frames N] [--replay io.log] [--iolog out.log]
 *             [--dump frames.bin] [--trace out.tr --trace-instr N] [--wav out.wav]
 *             [--snap dir --snap-frames a,b,c]   (PNG of the frame rendered at vblank N)
 *             [--input script]   lines "<vblank> <hex mask> [<hex mask P2>]", same as capture.lua
 *                                CAP_INPUT (which reads only P1's)
 *             [--coop]           two players (coop.c); P2's buttons are the scripts' second mask
 *             [--state-save-at N file] [--state-load file]
 *             [--filter mode]    write --snap PNGs through a CRT filter (crt.h), at 4x
 *             [--mods a,b [--mods-dir dir]]   runtime mods (mod.h); default dir "mods"
 *             [--vdump dir]      with --snap-frames: also write dir/vNNNNN.bin, the video state
 *                                at that vblank (PPU regs 0x100, video RAM 0x700, RAM 0x2800 words,
 *                                little-endian) for tools/gfx.py
 *             [--romcov out.bin] ROM usage map: per ROM-space word a byte of SPG_COV_* bits, then
 *                                its first vblank as u32 (tools/romcov.py reads it)
 *             [--bake out.bin]   write the ROM file with the mods' patches applied, for MAME
 *             [--hash]           print a hash of the final state (spg_snap_hash)
 *             [--host PORT | --join ADDR[:PORT]]   netplay (net.h), implies --coop: the host is P1
 *                                and starts from its state (--state-load or the reset), the guest
 *                                joins from it. Each plays its own column of --input (the host
 *                                the first mask, the guest the second) at 60 Hz (--fps N), up to
 *                                --frames N, then waits until both have every input and exits.
 *                                [--net-delay N] input delay (2), [--net-sim LAT,JITTER,LOSS%]
 *                                a simulated network (ms), [--net-log f] the settled inputs as an
 *                                input script, [--net-start f] the starting state, for replays
 *             [--watch out.txt spec] log CPU accesses as "vblank W|R pc addr value"; spec is a
 *                                comma list of w:LO-HI (writes), r:LO-HI (data reads), from:N
 *                                and to:N (vblanks), fn:ADDR (only inside calls of the function at
 *                                ADDR), numbers in hex except from/to
 *
 * --replay feeds MAME's recorded peripheral reads (tools/mame/capture.lua) back in, so the
 * CPU can be checked against MAME before the peripherals are modelled exactly. The outputs
 * use the same formats as capture.lua, for tests/compare.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop.h"
#include "crt.h"
#include "mod.h"
#include "net.h"
#include "png.h"
#include "rom.h"
#include "spg.h"
#include "state.h"

#include <time.h>

static FILE *dump_out;
static FILE *wav_out;
static const char *snap_dir;
static char snap_frames[4096];
static const char *vdump_dir;
static crt_t *snap_crt;
static int snap_filter;
static uint32_t *snap_buf;
static struct input_rec { uint32_t frame; uint16_t mask, mask2; } *inputs;
static int cap_inputs;
static int n_inputs;

static int load_inputs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    unsigned fr, mask, mask2;
    while (fgets(line, sizeof(line), f)) {
        const int k = sscanf(line, "%u %x %x", &fr, &mask, &mask2);
        if (k >= 2) {
            if (n_inputs == cap_inputs)
                inputs = realloc(inputs, (size_t)(cap_inputs = cap_inputs ? cap_inputs * 2 : 4096) * sizeof(*inputs));
            inputs[n_inputs].frame = fr;
            inputs[n_inputs].mask = (uint16_t)mask;
            inputs[n_inputs++].mask2 = k == 3 ? (uint16_t)mask2 : 0;
        }
    }
    fclose(f);
    return 1;
}
static uint32_t wav_samples;

static void wav_header(FILE *f, uint32_t samples)
{
    const uint32_t rate = SPU_RATE, bytes = samples * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + bytes; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = rate; *(uint32_t *)(h + 28) = rate * 4; *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

static void wav_sample(void *ctx, int16_t l, int16_t r)
{
    (void)ctx;
    int16_t lr[2] = { l, r };
    fwrite(lr, 2, 2, wav_out);
    wav_samples++;
}

static int want_snap(uint32_t frame)
{
    char key[16];
    snprintf(key, sizeof(key), ",%u,", frame);
    return strstr(snap_frames, key) != NULL;
}

/* after mods_frame, so that what mods draw is in the picture */
static void snap_frame(spg_t *m)
{
    if (vdump_dir && m->frame && want_snap(m->frame)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/v%05u.bin", vdump_dir, m->frame);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(m->vregs, 2, 0x100, f) != 0x100 || fwrite(m->vram, 2, 0x700, f) != 0x700 ||
            fwrite(m->ram, 2, 0x2800, f) != 0x2800 || fclose(f))
            perror(path);
    }
    if (snap_dir && m->frame && want_snap(m->frame)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/f%05u.png", snap_dir, m->frame);
        int ok;
        if (snap_crt) {
            crt_apply(snap_crt, snap_filter, m->framebuffer, snap_buf);
            ok = png_write_xrgb(path, snap_buf, crt_width(snap_crt), crt_height(snap_crt));
        } else {
            ok = png_write_xrgb(path, m->framebuffer, PPU_W, PPU_H);
        }
        if (!ok) perror(path);
    }
}

static void dump_frame(spg_t *m)
{
    static int next_input; /* scripts are in vblank order */
    while (next_input < n_inputs && inputs[next_input].frame <= m->frame) {
        if (inputs[next_input].frame == m->frame) {
            m->buttons = inputs[next_input].mask;
            m->coop.buttons2 = inputs[next_input].mask2;
        }
        next_input++;
    }
    if (!dump_out) return;
    uint32_t fr = m->frame;
    uint64_t cyc = (uint64_t)m->cpu.cycles;
    fwrite(&fr, 4, 1, dump_out);
    fwrite(&cyc, 8, 1, dump_out);
    fwrite(m->ram, 2, 0x2800, dump_out);
    fwrite(m->vram, 2, 0x700, dump_out);
}

/* the script's buttons for one player at frame f (the last line at or before f) */
static uint16_t script_at(uint32_t f, int p2)
{
    int lo = 0, hi = n_inputs;   /* first line after f */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (inputs[mid].frame <= f) lo = mid + 1;
        else hi = mid;
    }
    return lo ? (p2 ? inputs[lo - 1].mask2 : inputs[lo - 1].mask) : 0;
}

static uint32_t net_corrupt_at;   /* test: BATMAN_NET_CORRUPT=frame moves P1 in this process only */

static void net_run(void *ctx, spg_t *m, int render, int audio)
{
    (void)ctx; (void)render; (void)audio;
    spg_run_frame(m, NULL, NULL);
    mods_frame(m);
    if (m->frame == net_corrupt_at && m->ram[0x270A] && m->ram[0x270A] < 0x2800)
        m->ram[m->ram[0x270A]] += 8;   /* the player's X */
}

static void sleep_until(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL);
}

/* two processes play together; see the usage at the top */
static int run_netplay(spg_t *m, const uint8_t rom_hash[32], int host_port, const char *join,
                       const net_opts_t *o, unsigned frames, int fps, const char *log_path, const char *start_path)
{
    netplay_t *n;
    if (join) {
        char addr[256];
        int port = NET_PORT;
        snprintf(addr, sizeof(addr), "%s", join);
        char *c = strrchr(addr, ':');
        if (addr[0] == '[') {   /* [v6]:port */
            char *e = strchr(addr, ']');
            if (e) { *e = 0; if (e[1] == ':') port = atoi(e + 2); memmove(addr, addr + 1, strlen(addr)); }
        } else if (c && c == strchr(addr, ':')) { *c = 0; port = atoi(c + 1); }
        n = net_join(addr, port, o, rom_hash);
    } else n = net_host(host_port, o);
    if (!n) return 0;
    int r;
    while ((r = join ? net_wait_start(n, m) : net_accept(n, m, rom_hash)) == 0) {
        const struct timespec t = { 0, 5000000 };
        nanosleep(&t, NULL);
    }
    if (r < 0) { fprintf(stderr, "net: %s\n", net_status(n)); net_close(n); return 0; }
    if (start_path && !spg_state_save(m, rom_hash, start_path)) { net_close(n); return 0; }
    FILE *log = NULL;
    if (log_path) {
        if (!(log = fopen(log_path, "w"))) { perror(log_path); net_close(n); return 0; }
        net_log_inputs(n, log);
    }
    const int p2 = !net_is_host(n);
    if (getenv("BATMAN_NET_CORRUPT")) net_corrupt_at = (uint32_t)atoi(getenv("BATMAN_NET_CORRUPT"));
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int ok = 1, linger = 0;
    for (;;) {
        const uint16_t local = script_at(net_input_frame(n), p2);
        r = net_tick(n, m, local, frames, net_run, NULL);
        if (net_settled(n, m, frames)) {
            if (++linger > fps) break;   /* a second more, so the peer surely has our last acks */
        } else if (r < 0) {
            fprintf(stderr, "net: %s\n", net_status(n));
            ok = 0;
            break;
        }
        sleep_until(&t, 1000000000L / fps);
    }
    if (log) fclose(log);
    net_close(n);
    return ok;
}

/* the patched ROM as a file MAME can run; MAME mirrors the file, so a patched upper half is lost */
static int bake_rom(const uint16_t *rom, const char *path)
{
    if (memcmp(rom, rom + ROM_FILE_WORDS, ROM_FILE_WORDS * 2))
        fprintf(stderr, "bake: the upper ROM half is patched; MAME maps the file there again, so it won't see that\n");
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    for (size_t i = 0; i < ROM_FILE_WORDS; i++) {
        fputc(rom[i] & 0xff, f);
        fputc(rom[i] >> 8, f);
    }
    return !fclose(f);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *replay = NULL, *iolog = NULL, *dump = NULL, *trace_path = NULL;
    unsigned frames = 600;
    uint64_t trace_instr = 0;
    int coop = 0, print_hash = 0, host_port = 0, fps = 60;
    const char *join = NULL, *net_log = NULL, *net_start = NULL;
    net_opts_t net_o = { 2, 0, 0, 0 };
    const char *state_save = NULL, *state_load = NULL, *mod_list = NULL, *mods_dir = "mods", *bake = NULL, *romcov = NULL;
    const char *watch = NULL, *watch_spec = NULL;
    FILE *watch_out = NULL;
    unsigned watch_from = 0, watch_to = ~0u;
    unsigned state_save_at = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "--iolog") && i + 1 < argc) iolog = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--trace-instr") && i + 1 < argc) trace_instr = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) snap_dir = argv[++i];
        else if (!strcmp(argv[i], "--state-save-at") && i + 2 < argc) {
            state_save_at = (unsigned)atoi(argv[++i]);
            state_save = argv[++i];
        }
        else if (!strcmp(argv[i], "--state-load") && i + 1 < argc) state_load = argv[++i];
        else if (!strcmp(argv[i], "--mods") && i + 1 < argc) mod_list = argv[++i];
        else if (!strcmp(argv[i], "--coop")) coop = 1;
        else if (!strcmp(argv[i], "--hash")) print_hash = 1;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--net-delay") && i + 1 < argc) net_o.delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--net-sim") && i + 1 < argc)
            sscanf(argv[++i], "%d,%d,%d", &net_o.lat_ms, &net_o.jitter_ms, &net_o.loss_pct);
        else if (!strcmp(argv[i], "--net-log") && i + 1 < argc) net_log = argv[++i];
        else if (!strcmp(argv[i], "--net-start") && i + 1 < argc) net_start = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mods-dir") && i + 1 < argc) mods_dir = argv[++i];
        else if (!strcmp(argv[i], "--bake") && i + 1 < argc) bake = argv[++i];
        else if (!strcmp(argv[i], "--romcov") && i + 1 < argc) romcov = argv[++i];
        else if (!strcmp(argv[i], "--watch") && i + 2 < argc) { watch = argv[++i]; watch_spec = argv[++i]; }
        else if (!strcmp(argv[i], "--vdump") && i + 1 < argc) vdump_dir = argv[++i];
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            if ((snap_filter = crt_mode_parse(argv[++i])) < 0) { fprintf(stderr, "unknown filter %s\n", argv[i]); return 2; }
            snap_crt = crt_new(4);
            snap_buf = malloc(sizeof(uint32_t) * crt_width(snap_crt) * crt_height(snap_crt));
        }
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (!load_inputs(argv[++i])) { perror(argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--snap-frames") && i + 1 < argc) snprintf(snap_frames, sizeof(snap_frames), ",%s,", argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            if (!(wav_out = fopen(argv[++i], "wb"))) { perror(argv[i]); return 1; }
            wav_header(wav_out, 0);
        }
        else if (argv[i][0] != '-') rom_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s <batman.bin> [--frames N] [--replay io.log] [--iolog out] [--dump out] [--trace out --trace-instr N] [--wav out.wav] [--snap dir --snap-frames a,b]\n", argv[0]);
        return 2;
    }

    uint8_t file_sha[32];
    uint16_t *rom = rom_load(rom_path, file_sha);
    if (!rom) return 1;
    if (mod_list && !mods_load(mod_list, mods_dir, rom, file_sha)) return 1;
    if (bake && !bake_rom(rom, bake)) return 1;

    static spg_t m;
    spg_init(&m, rom);
    if (host_port || join) coop = 1;
    if (coop) coop_attach(&m);
    mods_attach(&m);
    if (romcov) {
        m.romcov = calloc(ROM_SPACE_WORDS, 1);
        m.romcov_first = calloc(ROM_SPACE_WORDS, 4);
    }
    if (watch) {
        if (!(watch_out = fopen(watch, "w"))) { perror(watch); return 1; }
        m.watch_wlo = m.watch_rlo = 1; /* empty ranges */
        char spec[256];
        snprintf(spec, sizeof(spec), "%s", watch_spec);
        for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
            unsigned lo, hi;
            if (sscanf(t, "w:%x-%x", &lo, &hi) == 2) { m.watch_wlo = lo; m.watch_whi = hi; }
            else if (sscanf(t, "r:%x-%x", &lo, &hi) == 2) { m.watch_rlo = lo; m.watch_rhi = hi; }
            else if (sscanf(t, "from:%u", &lo) == 1) watch_from = lo;
            else if (sscanf(t, "to:%u", &lo) == 1) watch_to = lo;
            else if (sscanf(t, "fn:%x", &lo) == 1) m.watch_fn = lo;
            else { fprintf(stderr, "--watch: bad spec item '%s'\n", t); return 1; }
        }
    }
    if (replay && !spg_load_replay(&m, replay)) { perror(replay); return 1; }
    if (iolog && !(m.iolog = fopen(iolog, "w"))) { perror(iolog); return 1; }
    if (dump && !(dump_out = fopen(dump, "wb"))) { perror(dump); return 1; }
    FILE *trace = NULL;
    if (trace_path && !(trace = fopen(trace_path, "w"))) { perror(trace_path); return 1; }
    if (trace && !trace_instr) trace_instr = UINT64_MAX;
    m.on_frame = dump_frame;
    if (wav_out) m.spu.out = wav_sample;
    m.render = snap_dir != NULL || getenv("BATMAN_RENDER") != NULL;

    uint8_t rom_hash[32];
    mods_content_id(rom, rom_hash);
    spg_reset(&m);
    mods_reset(&m);
    if (state_load) {
        if (!spg_state_load(&m, rom_hash, state_load)) return 1;
    } else {
        dump_frame(&m); /* frame 0: MAME's initial frame_update, before the CPU runs */
    }

    int ok = 1;
    if (host_port || join) {
        m.on_frame = NULL;   /* the inputs come from net.c */
        ok = run_netplay(&m, rom_hash, host_port, join, &net_o, frames, fps > 0 ? fps : 60, net_log, net_start);
    }
    while (!host_port && !join && m.frame < frames) {
        if (state_save && m.frame == state_save_at && !spg_state_save(&m, rom_hash, state_save)) return 1;
        if (watch_out) m.watch = m.frame + 1 >= watch_from && m.frame + 1 <= watch_to ? watch_out : NULL;
        if (!spg_run_frame(&m, trace, &trace_instr)) { ok = 0; break; }
        mods_frame(&m);
        snap_frame(&m);
    }
    if (state_save && m.frame == state_save_at && !spg_state_save(&m, rom_hash, state_save)) return 1;

    if (m.replay) {
        fprintf(stderr, "replay: %zu/%zu reads consumed%s\n", m.replay_pos, m.replay_len,
                m.replay_diverged ? " (diverged)" : "");
        for (int a = 0; a < 0x1800; a++)
            if (m.model_mismatch[a])
                fprintf(stderr, "  model differs from MAME at %04X: %u reads\n", 0x2800 + a, m.model_mismatch[a]);
    }
    fprintf(stderr, "ran %u frames, %lld cycles, pc %06x\n", m.frame, (long long)m.cpu.cycles, unsp_lpc(&m.cpu));
    if (print_hash) {
        static spg_snap_t s;
        spg_snapshot(&m, &s);
        printf("state %016llx at frame %u\n", (unsigned long long)spg_snap_hash(&s), m.frame);
    }
    if (romcov) {
        FILE *f = fopen(romcov, "wb");
        if (!f || fwrite(m.romcov, 1, ROM_SPACE_WORDS, f) != ROM_SPACE_WORDS ||
            fwrite(m.romcov_first, 4, ROM_SPACE_WORDS, f) != ROM_SPACE_WORDS || fclose(f)) {
            perror(romcov);
            ok = 0;
        }
    }
    if (m.iolog) fclose(m.iolog);
    if (watch_out) fclose(watch_out);
    if (dump_out) fclose(dump_out);
    if (wav_out) { wav_header(wav_out, wav_samples); fclose(wav_out); }
    if (trace) fclose(trace);
    return ok && !m.replay_diverged ? 0 : 1;
}
