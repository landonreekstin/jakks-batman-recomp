/* Runtime mods: mods/<name>/mod.txt, applied at load time; the ROM file is never changed.
 *
 * mod.txt is one directive per line ('#' starts a comment). Addresses are µ'nSP word
 * addresses; numbers are hex (0x optional).
 *   name   <free text>
 *   rom    <sha256 of the original batman.bin>      required
 *   patch  <addr> <file>        overwrite ROM words from a file of little-endian words
 *   words  <addr> <w> <w> ...   overwrite ROM words given inline
 *   text   <addr> "string"      overwrite ROM words with the characters, one per word
 *   poke   <addr> <value> [always]   RAM write after reset (or at every vblank)
 *   plugin <file.so>            native plugin, see mod_api.h
 * ROM addresses run 0x000000-0x3fffff: the upper half starts as a copy of the lower one
 * and can be patched separately (free space, if the game never reads it). */
#ifndef MOD_H
#define MOD_H

#include <stdint.h>

#include "spg.h"

/* Loads and applies the comma-separated mods in `list` (names under mods_dir, or paths
 * containing '/'). Patches go into `rom` (ROM_SPACE_WORDS). Returns 0 after printing the
 * reason if a mod is broken or made for a different ROM. */
int mods_load(const char *list, const char *mods_dir, uint16_t *rom, const uint8_t rom_file_sha256[32]);

/* reads mods_dir/enabled.txt (one name per line) into a comma list; 0 if there is none */
int mods_enabled_list(const char *mods_dir, char *out, size_t outlen);

/* identifies the ROM space plus the loaded mods, for save states */
void mods_content_id(const uint16_t *rom, uint8_t out[32]);

/* wire the machine to the loaded mods (hooks); call after spg_init */
void mods_attach(spg_t *m);
/* after spg_reset: one-shot pokes */
void mods_reset(spg_t *m);
/* after every spg_run_frame: per-frame pokes and plugin frame callbacks */
void mods_frame(spg_t *m);

int mods_count(void);

#endif
