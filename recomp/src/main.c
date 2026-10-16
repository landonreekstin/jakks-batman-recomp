/* Headless runner for the jak_batm reconstruction.
 *
 *   batman-rt <batman.bin> [--frames N] [--replay io.log] [--iolog out.log]
 *             [--dump frames.bin] [--trace out.tr --trace-instr N] [--wav out.wav]
 *             [--snap dir --snap-frames a,b,c]   (PNG of the frame rendered at vblank N)
 *             [--input script]   lines "<vblank> <hex mask>", same as capture.lua CAP_INPUT
 *             [--state-save-at N file] [--state-load file]
 *
 * --replay feeds MAME's recorded peripheral reads (tools/mame/capture.lua) back in, so the
 * CPU can be checked against MAME before the peripherals are modelled exactly. The outputs
 * use the same formats as capture.lua, for tests/compare.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include "spg.h"
#include "state.h"

#define ROM_WORDS 0x200000

static FILE *dump_out;
static FILE *wav_out;
static const char *snap_dir;
static char snap_frames[4096];
static struct input_rec { uint32_t frame; uint16_t mask; } *inputs;
static int cap_inputs;
static int n_inputs;

static int load_inputs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    unsigned fr, mask;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%u %x", &fr, &mask) == 2) {
            if (n_inputs == cap_inputs)
                inputs = realloc(inputs, (size_t)(cap_inputs = cap_inputs ? cap_inputs * 2 : 4096) * sizeof(*inputs));
            inputs[n_inputs].frame = fr;
            inputs[n_inputs++].mask = (uint16_t)mask;
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

static void dump_frame(spg_t *m)
{
    static int next_input; /* scripts are in vblank order */
    while (next_input < n_inputs && inputs[next_input].frame <= m->frame) {
        if (inputs[next_input].frame == m->frame) m->buttons = inputs[next_input].mask;
        next_input++;
    }
    if (snap_dir && m->frame && want_snap(m->frame)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/f%05u.png", snap_dir, m->frame);
        if (!png_write_xrgb(path, m->framebuffer, PPU_W, PPU_H)) perror(path);
    }
    if (!dump_out) return;
    uint32_t fr = m->frame;
    uint64_t cyc = (uint64_t)m->cpu.cycles;
    fwrite(&fr, 4, 1, dump_out);
    fwrite(&cyc, 8, 1, dump_out);
    fwrite(m->ram, 2, 0x2800, dump_out);
    fwrite(m->vram, 2, 0x700, dump_out);
}

static uint16_t *load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint16_t *rom = calloc(ROM_WORDS, 2);
    uint8_t *raw = malloc(ROM_WORDS * 2);
    size_t n = fread(raw, 1, ROM_WORDS * 2, f);
    fclose(f);
    if (n != ROM_WORDS * 2) { fprintf(stderr, "%s: expected %d bytes, got %zu\n", path, ROM_WORDS * 2, n); return NULL; }
    for (size_t i = 0; i < ROM_WORDS; i++) rom[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    free(raw);
    return rom;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *replay = NULL, *iolog = NULL, *dump = NULL, *trace_path = NULL;
    unsigned frames = 600;
    uint64_t trace_instr = 0;
    const char *state_save = NULL, *state_load = NULL;
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

    uint16_t *rom = load_rom(rom_path);
    if (!rom) return 1;

    static spg_t m;
    spg_init(&m, rom);
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
    spg_rom_hash(rom, rom_hash);
    spg_reset(&m);
    if (state_load) {
        if (!spg_state_load(&m, rom_hash, state_load)) return 1;
    } else {
        dump_frame(&m); /* frame 0: MAME's initial frame_update, before the CPU runs */
    }

    int ok = 1;
    while (m.frame < frames) {
        if (state_save && m.frame == state_save_at && !spg_state_save(&m, rom_hash, state_save)) return 1;
        if (!spg_run_frame(&m, trace, &trace_instr)) { ok = 0; break; }
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
    if (m.iolog) fclose(m.iolog);
    if (dump_out) fclose(dump_out);
    if (wav_out) { wav_header(wav_out, wav_samples); fclose(wav_out); }
    if (trace) fclose(trace);
    return ok && !m.replay_diverged ? 0 : 1;
}
