/* µ'nSP ISA 1.0 core (the CPU inside the SPG24x).
 * Semantics and cycle counts follow MAME 0.287 src/devices/cpu/unsp so that
 * traces can be diffed against MAME instruction by instruction. */
#ifndef UNSP_H
#define UNSP_H

#include <stdint.h>

enum { REG_SP, REG_R1, REG_R2, REG_R3, REG_R4, REG_BP, REG_SR, REG_PC };

#define UNSP_N 0x0200
#define UNSP_Z 0x0100
#define UNSP_S 0x0080
#define UNSP_C 0x0040

/* input lines, same numbering as MAME: bit 0 = FIQ, bits 1..8 = IRQ0..IRQ7 */
enum { UNSP_FIQ_LINE = 0, UNSP_IRQ0_LINE = 1 };

typedef struct unsp {
    uint16_t r[8];
    uint16_t secbank[4];
    uint8_t enable_irq, enable_fiq, fir_move, sb, aq, fra, bnk, ine, pri;
    uint8_t in_fiq, in_irq;
    uint16_t sirq;          /* asserted input lines */
    int64_t cycles;         /* total cycles executed, 27 MHz */

    /* bus, supplied by the machine */
    void *ctx;
    uint16_t (*read)(void *ctx, uint32_t addr);
    void (*write)(void *ctx, uint32_t addr, uint16_t data);
} unsp_t;

static inline uint32_t unsp_lpc(const unsp_t *c)
{
    return ((uint32_t)(c->r[REG_SR] & 0x3f) << 16) | c->r[REG_PC];
}

void unsp_reset(unsp_t *c);
void unsp_set_line(unsp_t *c, int line, int state);
/* Executes one instruction, then services pending interrupts like MAME's execute_run.
 * Returns the opcode word executed. */
uint16_t unsp_step(unsp_t *c);
/* Services pending interrupts as unsp_step does after an instruction (for code that
 * stands in for an instruction, such as a mod hook that replaced one). */
void unsp_check_irqs(unsp_t *c);
/* Sets non-zero when an opcode MAME treats as unimplemented on ISA 1.0 was hit. */
extern int unsp_bad_opcode;

#endif
