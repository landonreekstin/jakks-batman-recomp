/* 24C04 I2C EEPROM. See i2c.h. */
#include "i2c.h"

#include <string.h>

enum { IDLE, DEVSEL, ADDRESSLOW, DATAIN, READSELACK, DATAOUT, RESET };

#define SLAVE_ADDRESS 0xa0
#define WRITE_PAGE    16
#define ADDR_MASK     (EEPROM_SIZE - 1)

void eeprom_init(eeprom_t *e)
{
    memset(e, 0, sizeof(*e));
    memset(e->data, 0xff, sizeof(e->data));
    e->sdar = 1;
    e->state = IDLE;
}

static int select_device(const eeprom_t *e)
{
    /* A8 travels in devsel bit 1, so it is masked out of the comparison */
    const int mask = 0xfe & ~(ADDR_MASK >> 7);
    return (e->devsel & mask) == (SLAVE_ADDRESS & mask);
}

void eeprom_write_sda(eeprom_t *e, int state)
{
    state &= 1;
    if (e->sdaw == state) return;
    e->sdaw = state;
    if (!(e->scl && e->sdar)) return; /* ignored while the chip drives SDA low */
    if (e->sdaw) { /* stop: commit the page buffer */
        if (e->page_written_size > 0) {
            const int base = e->byteaddr & ADDR_MASK;
            const int root = base & ~(WRITE_PAGE - 1);
            for (int i = 0; i < e->page_written_size; i++)
                e->data[root | ((base + i) & (WRITE_PAGE - 1))] = e->page[i];
            e->page_written_size = 0;
            e->dirty = 1;
        }
        e->state = IDLE;
    } else { /* start */
        e->state = DEVSEL;
        e->bits = 0;
    }
}

void eeprom_write_scl(eeprom_t *e, int state)
{
    if (e->scl == state) return;
    e->scl = state;

    switch (e->state) {
    case DEVSEL: case ADDRESSLOW: case DATAIN:
        if (e->bits < 8) {
            if (e->scl) {
                e->shift = (uint8_t)((e->shift << 1) | e->sdaw);
                e->bits++;
            }
        } else if (e->scl) {
            e->bits++;
        } else if (e->bits == 8) {
            switch (e->state) {
            case DEVSEL:
                e->devsel = e->shift;
                if (e->devsel == 0) e->state = RESET;
                else if (!select_device(e)) e->state = IDLE;
                else if ((e->devsel & 1) == 0) e->state = ADDRESSLOW;
                else e->state = READSELACK;
                break;
            case ADDRESSLOW:
                e->byteaddr = e->shift | (((e->devsel & 0xfe) << 7) & ADDR_MASK);
                e->page_offset = 0;
                e->page_written_size = 0;
                e->state = DATAIN;
                break;
            case DATAIN:
                e->page[e->page_offset] = e->shift;
                if (++e->page_offset == WRITE_PAGE) e->page_offset = 0;
                if (++e->page_written_size > WRITE_PAGE) e->page_written_size = WRITE_PAGE;
                break;
            }
            if (e->state != IDLE) e->sdar = 0; /* acknowledge */
        } else {
            e->bits = 0;
            e->sdar = 1;
        }
        break;

    case READSELACK:
        e->bits = 0;
        e->state = DATAOUT;
        break;

    case DATAOUT:
        if (e->bits < 8) {
            if (e->scl) {
                e->bits++;
            } else {
                if (e->bits == 0) {
                    e->shift = e->data[e->byteaddr & ADDR_MASK];
                    e->byteaddr++; /* read page size 0: sequential reads run through the chip */
                }
                e->sdar = (e->shift >> 7) & 1;
                e->shift = (uint8_t)(e->shift << 1);
            }
        } else if (e->scl) {
            if (e->sdaw) e->state = IDLE; /* no acknowledge from the master */
            e->bits = 0;
        } else {
            e->sdar = 1;
        }
        break;

    case RESET:
        if (e->scl) {
            if (e->bits > 8) {
                e->state = IDLE;
                e->sdar = 1;
            }
            e->bits++;
        }
        break;
    }
}

int eeprom_read_sda(const eeprom_t *e)
{
    /* an idle chip does not drive the line: the master reads back its own value */
    return e->state == IDLE ? (e->sdaw & 1) : (e->sdar & 1);
}
