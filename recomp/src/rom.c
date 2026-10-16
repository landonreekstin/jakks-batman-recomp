/* ROM loading: see rom.h. */
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

uint16_t *rom_load(const char *path, uint8_t file_sha256[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t *raw = malloc(ROM_FILE_WORDS * 2);
    const size_t n = fread(raw, 1, ROM_FILE_WORDS * 2, f);
    fclose(f);
    if (n != ROM_FILE_WORDS * 2) {
        fprintf(stderr, "%s: expected %d bytes, got %zu\n", path, ROM_FILE_WORDS * 2, n);
        free(raw);
        return NULL;
    }
    if (file_sha256) sha256(raw, n, file_sha256);
    uint16_t *rom = malloc(ROM_SPACE_WORDS * 2);
    for (size_t i = 0; i < ROM_FILE_WORDS; i++) rom[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    memcpy(rom + ROM_FILE_WORDS, rom, ROM_FILE_WORDS * 2);
    free(raw);
    return rom;
}
