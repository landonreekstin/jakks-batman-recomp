/* 24C04 I2C EEPROM (512 bytes, 16-byte write pages) holding the saved games.
 * Bit-level behaviour follows MAME 0.287 src/devices/machine/i2cmem.cpp. */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

#define EEPROM_SIZE 0x200

typedef struct eeprom {
    uint8_t data[EEPROM_SIZE];
    uint8_t page[16];
    int scl, sdaw, sdar, state, bits;
    uint8_t shift, devsel;
    int byteaddr, page_offset, page_written_size;
    int dirty;
} eeprom_t;

void eeprom_init(eeprom_t *e); /* blank chip: all 0xff */
void eeprom_write_sda(eeprom_t *e, int state);
void eeprom_write_scl(eeprom_t *e, int state);
int eeprom_read_sda(const eeprom_t *e);

#endif
