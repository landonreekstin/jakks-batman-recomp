/* Save states: see state.h. */
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom.h"
#include "sha256.h"

#define STATE_MAGIC "BATSTATE"
#define STATE_VERSION 5   /* 2: co-op state; 3: its per-level addresses; 4: doors; 5: search */

typedef struct state_header {
    char magic[8];
    uint32_t version;
    uint32_t snap_size;
    uint8_t rom_hash[32];
    uint32_t frame;
    uint32_t reserved;
} state_header_t;

void spg_snapshot(const spg_t *m, spg_snap_t *s)
{
    s->cpu = m->cpu;
    s->cpu.ctx = NULL;
    s->cpu.read = NULL;
    s->cpu.write = NULL;
    memcpy(s->ram, m->ram, sizeof(s->ram));
    memcpy(s->vregs, m->vregs, sizeof(s->vregs));
    memcpy(s->vram, m->vram, sizeof(s->vram));
    s->spu = m->spu;
    s->spu.ctx = NULL;
    s->spu.read = NULL;
    s->spu.out = NULL;
    memcpy(s->ycmp_table, m->ppu.ycmp_table, sizeof(s->ycmp_table));
    memcpy(s->ioregs, m->ioregs, sizeof(s->ioregs));
    memcpy(s->dmaregs, m->dmaregs, sizeof(s->dmaregs));
    s->buttons = m->buttons;
    s->eeprom = m->eeprom;
    s->frame = m->frame;
    s->next_vbl_begin = m->next_vbl_begin;
    s->next_vbl_end = m->next_vbl_end;
    s->vbl_end_count = m->vbl_end_count;
    s->next_snd_update = m->next_snd_update;
    s->snd_update_count = m->snd_update_count;
    s->next_rng_tick = m->next_rng_tick;
    s->rng_tick_count = m->rng_tick_count;
    s->vblank_start_as = m->vblank_start_as;
    s->coop = m->coop;
}

void spg_restore(spg_t *m, const spg_snap_t *s)
{
    const unsp_t cpu = m->cpu;
    m->cpu = s->cpu;
    m->cpu.ctx = cpu.ctx;
    m->cpu.read = cpu.read;
    m->cpu.write = cpu.write;
    memcpy(m->ram, s->ram, sizeof(s->ram));
    memcpy(m->vregs, s->vregs, sizeof(s->vregs));
    memcpy(m->vram, s->vram, sizeof(s->vram));
    const spu_t spu = m->spu;
    m->spu = s->spu;
    m->spu.ctx = spu.ctx;
    m->spu.read = spu.read;
    m->spu.out = spu.out;
    memcpy(m->ppu.ycmp_table, s->ycmp_table, sizeof(s->ycmp_table));
    m->ppu.lut_valid = 0;
    memcpy(m->ioregs, s->ioregs, sizeof(s->ioregs));
    memcpy(m->dmaregs, s->dmaregs, sizeof(s->dmaregs));
    m->buttons = s->buttons;
    m->eeprom = s->eeprom;
    m->eeprom.dirty = 1; /* the save file should follow the restored EEPROM */
    m->frame = s->frame;
    m->next_vbl_begin = s->next_vbl_begin;
    m->next_vbl_end = s->next_vbl_end;
    m->vbl_end_count = s->vbl_end_count;
    m->next_snd_update = s->next_snd_update;
    m->snd_update_count = s->snd_update_count;
    m->next_rng_tick = s->next_rng_tick;
    m->rng_tick_count = s->rng_tick_count;
    m->vblank_start_as = s->vblank_start_as;
    const int coop_on = m->coop.enabled; /* whether this process has co-op's hooks, not the state's */
    m->coop = s->coop;
    m->coop.enabled = coop_on;
    if (m->render) spg_render(m);
}

void spg_rom_hash(const uint16_t *rom, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    uint8_t buf[4096];
    for (size_t i = 0; i < ROM_SPACE_WORDS; i += sizeof(buf) / 2) {
        for (size_t j = 0; j < sizeof(buf) / 2; j++) {
            buf[2 * j] = (uint8_t)rom[i + j];
            buf[2 * j + 1] = (uint8_t)(rom[i + j] >> 8);
        }
        sha256_update(&s, buf, sizeof(buf));
    }
    sha256_final(&s, out);
}

int spg_state_save(const spg_t *m, const uint8_t rom_hash[32], const char *path)
{
    spg_snap_t *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    spg_snapshot(m, s);
    state_header_t h = { .version = STATE_VERSION, .snap_size = sizeof(*s), .frame = m->frame };
    memcpy(h.magic, STATE_MAGIC, 8);
    memcpy(h.rom_hash, rom_hash, 32);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(s, sizeof(*s), 1, f) == 1;
    if (f && fclose(f)) ok = 0;
    if (!ok) perror(path);
    free(s);
    return ok;
}

int spg_state_load(spg_t *m, const uint8_t rom_hash[32], const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    state_header_t h;
    spg_snap_t *s = calloc(1, sizeof(*s));
    int ok = 0;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, STATE_MAGIC, 8))
        fprintf(stderr, "%s: not a save state\n", path);
    else if (h.version != STATE_VERSION || h.snap_size != sizeof(*s))
        fprintf(stderr, "%s: made by a different build (version %u, %u bytes)\n", path, h.version, h.snap_size);
    else if (memcmp(h.rom_hash, rom_hash, 32))
        fprintf(stderr, "%s: made with a different ROM image or mod set\n", path);
    else if (fread(s, sizeof(*s), 1, f) != 1)
        fprintf(stderr, "%s: truncated\n", path);
    else {
        spg_restore(m, s);
        ok = 1;
    }
    fclose(f);
    free(s);
    return ok;
}
