/* Headless runner for the jak_batm reconstruction.
 *
 *   batman-rt <batman.bin> [--frames N] [--replay io.log] [--iolog out.log]
 *             [--dump frames.bin] [--trace out.tr --trace-instr N]
 *
 * --replay feeds MAME's recorded peripheral reads (tools/mame/capture.lua) back in, so the
 * CPU can be checked against MAME before the peripherals are modelled exactly. The outputs
 * use the same formats as capture.lua, for tests/compare.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spg.h"

#define ROM_WORDS 0x200000

static FILE *dump_out;

static void dump_frame(spg_t *m)
{
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

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "--iolog") && i + 1 < argc) iolog = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--trace-instr") && i + 1 < argc) trace_instr = strtoull(argv[++i], NULL, 0);
        else if (argv[i][0] != '-') rom_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s <batman.bin> [--frames N] [--replay io.log] [--iolog out] [--dump out] [--trace out --trace-instr N]\n", argv[0]);
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

    spg_reset(&m);
    dump_frame(&m); /* frame 0: MAME's initial frame_update, before the CPU runs */

    int ok = 1;
    while (m.frame < frames) {
        if (!spg_run_frame(&m, trace, &trace_instr)) { ok = 0; break; }
    }

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
    if (trace) fclose(trace);
    return ok && !m.replay_diverged ? 0 : 1;
}
