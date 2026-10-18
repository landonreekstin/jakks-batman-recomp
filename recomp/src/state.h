/* Save states for the SPG24x machine.
 *
 * A snapshot holds everything that changes while the game runs (CPU, RAM, video and sound
 * registers, EEPROM, the event schedule) and nothing derived: the framebuffer and the PPU
 * colour LUT are rebuilt from it. Snapshots are only taken between spg_run_frame calls,
 * i.e. at a vblank, so restoring one and running on is the same as never having stopped.
 *
 * The file format is a raw copy of spg_snap_t behind a header, so state files only load
 * into the same build layout (the header carries the version and size and a mismatch is
 * refused) and the same ROM image, identified by the SHA-256 of the image as loaded
 * (mods included). */
#ifndef STATE_H
#define STATE_H

#include <stdint.h>

#include "spg.h"

typedef struct spg_snap {
    unsp_t cpu;             /* pointer fields are not restored */
    uint16_t ram[0x2800];
    uint16_t vregs[0x100];
    uint16_t vram[0x700];
    spu_t spu;              /* pointer fields are not restored */
    uint32_t ycmp_table[480]; /* PPU state: rebuilt only on register writes, as in MAME */
    uint16_t ioregs[0x100];
    uint16_t dmaregs[4];
    uint16_t buttons;
    eeprom_t eeprom;
    uint32_t frame;
    int64_t next_vbl_begin, next_vbl_end;
    uint32_t vbl_end_count;
    int64_t next_snd_update;
    uint32_t snd_update_count;
    int64_t next_rng_tick;
    uint32_t rng_tick_count;
    unsigned __int128 vblank_start_as;
    coop_t coop;            /* two-player state (coop.c) */
} spg_snap_t;

void spg_snapshot(const spg_t *m, spg_snap_t *s);
/* restores the machine state and re-renders the framebuffer if m->render is set */
void spg_restore(spg_t *m, const spg_snap_t *s);

/* a hash of the game state (RAM, video, P2's co-op words, frame), to compare two machines */
uint64_t spg_snap_hash(const spg_snap_t *s);

/* SHA-256 of the ROM space as the machine sees it (ROM_SPACE_WORDS little-endian words) */
void spg_rom_hash(const uint16_t *rom, uint8_t out[32]);

/* return 0 and print the reason on failure */
int spg_state_save(const spg_t *m, const uint8_t rom_hash[32], const char *path);
int spg_state_load(spg_t *m, const uint8_t rom_hash[32], const char *path);

#endif
