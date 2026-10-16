/* The ROM as the CPU sees it: the 4 MiB file (0x200000 little-endian words) at word 0,
 * and the same content again at 0x200000 (MAME maps the file there too). The two halves
 * are separate copies so that mods can put new data in the upper one; unmodded, they
 * are identical and behave as MAME's mirror. */
#ifndef ROM_H
#define ROM_H

#include <stdint.h>

#define ROM_FILE_WORDS 0x200000
#define ROM_SPACE_WORDS 0x400000

/* returns a malloc'd ROM_SPACE_WORDS buffer, or NULL after printing why;
 * file_sha256 receives the hash of the file itself */
uint16_t *rom_load(const char *path, uint8_t file_sha256[32]);

#endif
