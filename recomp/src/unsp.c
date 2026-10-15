/* µ'nSP ISA 1.0 interpreter. See unsp.h. */
#include "unsp.h"

#include <stdio.h>

int unsp_bad_opcode;

#define R(n)  (c->r[n])
#define RD(a) (c->read(c->ctx, (a)))
#define WR(a, d) (c->write(c->ctx, (a), (d)))

static void add_lpc(unsp_t *c, int32_t off)
{
    uint32_t lpc = unsp_lpc(c) + (uint32_t)off;
    R(REG_PC) = (uint16_t)lpc;
    R(REG_SR) = (R(REG_SR) & 0xffc0) | ((lpc >> 16) & 0x3f);
}

static uint16_t fetch(unsp_t *c)
{
    uint16_t w = RD(unsp_lpc(c));
    add_lpc(c, 1);
    return w;
}

/* address in the data segment held in SR[15:10] */
static uint32_t lreg_i(unsp_t *c, int reg)
{
    return (((uint32_t)R(REG_SR) << 6) & 0x3f0000) | R(reg);
}

static void push(unsp_t *c, uint16_t v, int reg)
{
    WR(R(reg), v);
    R(reg) = (uint16_t)(R(reg) - 1);
}

static uint16_t pop(unsp_t *c, int reg)
{
    R(reg) = (uint16_t)(R(reg) + 1);
    return RD(R(reg));
}

static void update_nzsc(unsp_t *c, uint32_t value, uint16_t r0, uint16_t r1)
{
    uint16_t sr = R(REG_SR) & ~(UNSP_N | UNSP_Z | UNSP_S | UNSP_C);
    if (((value >> 16) & 1) != (((r0 ^ r1) >> 15) & 1)) sr |= UNSP_S;
    if (value & 0x8000) sr |= UNSP_N;
    if ((uint16_t)value == 0) sr |= UNSP_Z;
    if (value & 0x10000) sr |= UNSP_C;
    R(REG_SR) = sr;
}

static void update_nz(unsp_t *c, uint32_t value)
{
    uint16_t sr = R(REG_SR) & ~(UNSP_N | UNSP_Z);
    if (value & 0x8000) sr |= UNSP_N;
    if ((uint16_t)value == 0) sr |= UNSP_Z;
    R(REG_SR) = sr;
}

static void bad(unsp_t *c, uint16_t op)
{
    if (!unsp_bad_opcode)
        fprintf(stderr, "unsp: unimplemented opcode %04x at %06x\n", op, unsp_lpc(c) - 1);
    unsp_bad_opcode = 1;
}

void unsp_reset(unsp_t *c)
{
    for (int i = 0; i < 8; i++) R(i) = 0;
    for (int i = 0; i < 4; i++) c->secbank[i] = 0;
    R(REG_PC) = RD(0xfff0 + 7);
    c->enable_irq = c->enable_fiq = 0;
    c->fir_move = 1;
    c->sb = c->aq = c->fra = c->bnk = c->ine = 0;
    c->pri = 8;
    c->in_fiq = c->in_irq = 0;
    c->sirq = 0;
    c->cycles = 0;
    unsp_bad_opcode = 0;
}

void unsp_set_line(unsp_t *c, int line, int state)
{
    c->sirq &= ~(1u << line);
    if (state) c->sirq |= 1u << line;
}

static uint16_t get_fr(unsp_t *c)
{
    return (uint16_t)(c->aq << 14 | c->bnk << 13 | c->fra << 12 | c->fir_move << 11 |
                      c->sb << 7 | c->enable_fiq << 6 | c->enable_irq << 5 | c->ine << 4 | c->pri);
}

static void set_fr(unsp_t *c, uint16_t fr)
{
    /* ISA 1.0 never reaches this (INE stays 0), kept for completeness */
    c->aq = (fr >> 14) & 1;
    c->bnk = (fr >> 13) & 1;
    c->fra = (fr >> 12) & 1;
    c->fir_move = (fr >> 11) & 1;
    c->sb = (fr >> 7) & 0xf;
    c->enable_fiq = (fr >> 6) & 1;
    c->enable_irq = (fr >> 5) & 1;
    c->ine = (fr >> 4) & 1;
    c->pri = fr & 0xf;
}

static void trigger_fiq(unsp_t *c)
{
    if (!c->enable_fiq || c->in_fiq) return;
    c->in_fiq = 1;
    push(c, R(REG_PC), REG_SP);
    push(c, R(REG_SR), REG_SP);
    R(REG_PC) = RD(0xfff0 + 6);
    R(REG_SR) = 0;
}

static void trigger_irq(unsp_t *c, int line)
{
    if ((c->ine == 0 && c->in_irq) || c->pri <= line || !c->enable_irq) return;
    c->in_irq = 1;
    push(c, R(REG_PC), REG_SP);
    push(c, R(REG_SR), REG_SP);
    if (c->ine) {
        push(c, get_fr(c), REG_SP);
        c->pri = (uint8_t)line;
    }
    R(REG_PC) = RD(0xfff0 + 8 + line);
    R(REG_SR) = 0;
}

static void check_irqs(unsp_t *c)
{
    if (!c->sirq) return;
    /* MAME only looks at the lowest asserted line */
    int line = 0;
    while (!(c->sirq & (1u << line))) line++;
    if (line == UNSP_FIQ_LINE) trigger_fiq(c);
    else trigger_irq(c, line - 1);
}

static void exec_jump(unsp_t *c, uint16_t op)
{
    const int op0 = (op >> 12) & 15, op1 = (op >> 6) & 7;
    const int32_t off = (op1 == 0) ? (op & 0x3f) : -(int32_t)(op & 0x3f);
    const uint16_t sr = R(REG_SR);
    int take;
    switch (op0) {
    case 0:  take = !(sr & UNSP_C); break;                          /* jb  */
    case 1:  take = !!(sr & UNSP_C); break;                         /* jae */
    case 2:  take = !(sr & UNSP_S); break;                          /* jge */
    case 3:  take = !!(sr & UNSP_S); break;                         /* jl  */
    case 4:  take = !(sr & UNSP_Z); break;                          /* jne */
    case 5:  take = !!(sr & UNSP_Z); break;                         /* je  */
    case 6:  take = !(sr & UNSP_N); break;                          /* jpl */
    case 7:  take = !!(sr & UNSP_N); break;                         /* jmi */
    case 8:  take = (sr & (UNSP_Z | UNSP_C)) != UNSP_C; break;      /* jbe */
    case 9:  take = (sr & (UNSP_Z | UNSP_C)) == UNSP_C; break;      /* ja  */
    case 10: take = !!(sr & (UNSP_Z | UNSP_S)); break;              /* jle */
    case 11: take = !(sr & (UNSP_Z | UNSP_S)); break;               /* jg  */
    case 12: take = ((sr & UNSP_N) >> 9) == ((sr & UNSP_S) >> 7); break; /* jvc */
    case 13: take = ((sr & UNSP_N) >> 9) != ((sr & UNSP_S) >> 7); break; /* jvs */
    case 14: add_lpc(c, off); c->cycles += 4; return;               /* jmp */
    default: bad(c, op); return;
    }
    if (take) { c->cycles += 4; add_lpc(c, off); }
    else c->cycles += 2;
}

static void exec_fxxx(unsp_t *c, uint16_t op)
{
    const int opa = (op >> 9) & 7, opb = op & 7;
    switch ((op >> 6) & 7) {
    case 0: { /* MUL us (signed opb * unsigned opa) */
        c->cycles += 12;
        uint32_t lres = (uint32_t)R(opa) * R(opb);
        if (R(opb) & 0x8000) lres -= (uint32_t)R(opa) << 16;
        R(REG_R4) = (uint16_t)(lres >> 16);
        R(REG_R3) = (uint16_t)lres;
        return;
    }
    case 1:
        if ((op & 0xf3c0) == 0xf040) { /* call A22 */
            c->cycles += 9;
            uint16_t lo = fetch(c);
            push(c, R(REG_PC), REG_SP);
            push(c, R(REG_SR), REG_SP);
            R(REG_PC) = lo;
            R(REG_SR) = (R(REG_SR) & 0xffc0) | (op & 0x3f);
            return;
        }
        bad(c, op);
        return;
    case 2:
        if ((op & 0xffc0) == 0xfe80) { /* jmpf A22 */
            c->cycles += 5;
            R(REG_PC) = RD(unsp_lpc(c));
            R(REG_SR) = (R(REG_SR) & 0xffc0) | (op & 0x3f);
            return;
        }
        bad(c, op);
        return;
    case 3: /* JMPR is ISA 1.2+ */
        bad(c, op);
        return;
    case 4:
        if ((op & 0xf1f8) == 0xf108) { /* MUL ss */
            c->cycles += 12;
            uint32_t lres = (uint32_t)R(opa) * R(opb);
            if (R(opb) & 0x8000) lres -= (uint32_t)R(opa) << 16;
            if (R(opa) & 0x8000) lres -= (uint32_t)R(opb) << 16;
            R(REG_R4) = (uint16_t)(lres >> 16);
            R(REG_R3) = (uint16_t)lres;
            return;
        }
        bad(c, op);
        return;
    case 5:
        c->cycles += 2;
        switch (op & 0xf1ff) {
        case 0xf140: c->enable_irq = 0; c->enable_fiq = 0; return; /* int off */
        case 0xf141: c->enable_irq = 1; c->enable_fiq = 0; return; /* int irq */
        case 0xf142: c->enable_irq = 0; c->enable_fiq = 1; return; /* int fiq */
        case 0xf143: c->enable_irq = 1; c->enable_fiq = 1; return; /* int fiq,irq */
        case 0xf144: c->fir_move = 1; return;
        case 0xf145: c->fir_move = 0; return;
        case 0xf148: c->enable_irq = 0; return;                     /* irq off */
        case 0xf149: c->enable_irq = 1; return;                     /* irq on */
        case 0xf14c: c->enable_fiq = 0; return;                     /* fiq off */
        case 0xf14e: c->enable_fiq = 1; return;                     /* fiq on */
        case 0xf165: case 0xf16d: case 0xf175: case 0xf17d: return;  /* nop */
        default: bad(c, op); return;
        }
    case 6: case 7: { /* MULS ss, size 1..16; MAME charges no cycles */
        int size = (op >> 3) & 7;
        if (((op >> 6) & 7) == 7) size += 8;
        else if (size == 0) size = 16;
        const uint16_t rdv = R(opa), rsv = R(opb);
        int64_t lres = 0;
        uint16_t values[16];
        for (int i = 0; i < size; i++) {
            values[i] = RD((uint32_t)rdv + i);
            const uint16_t rhs = RD((uint32_t)rsv + i);
            uint32_t tres = (uint32_t)values[i] * rhs;
            if (values[i] & 0x8000) tres -= (uint32_t)rhs << 16;
            if (rhs & 0x8000) tres -= (uint32_t)values[i] << 16;
            lres += (int32_t)tres;
        }
        c->sb = 0;
        if (c->fir_move)
            for (int i = size - 1; i > 0; i--) WR((uint32_t)rdv + i, values[i - 1]);
        R(opa) += size;
        R(opb) += size;
        R(REG_R4) = (uint16_t)(lres >> 16);
        R(REG_R3) = (uint16_t)lres;
        return;
    }
    }
}

/* returns 1 when the result should be written back */
static int alu(unsp_t *c, int op0, uint32_t *lres, uint16_t r0, uint16_t r1, uint32_t r2, int flags)
{
    uint32_t carry = (R(REG_SR) & UNSP_C) ? 1 : 0;
    switch (op0) {
    case 0x0: *lres = (uint32_t)r0 + r1; if (flags) update_nzsc(c, *lres, r0, r1); return 1;
    case 0x1: *lres = (uint32_t)r0 + r1 + carry; if (flags) update_nzsc(c, *lres, r0, r1); return 1;
    case 0x2: *lres = (uint32_t)r0 + (uint16_t)~r1 + 1u; if (flags) update_nzsc(c, *lres, r0, (uint16_t)~r1); return 1;
    case 0x3: *lres = (uint32_t)r0 + (uint16_t)~r1 + carry; if (flags) update_nzsc(c, *lres, r0, (uint16_t)~r1); return 1;
    case 0x4: *lres = (uint32_t)r0 + (uint16_t)~r1 + 1u; if (flags) update_nzsc(c, *lres, r0, (uint16_t)~r1); return 0;
    case 0x6: *lres = (uint32_t)-(int32_t)r1; if (flags) update_nz(c, *lres); return 1;
    case 0x8: *lres = r0 ^ r1; if (flags) update_nz(c, *lres); return 1;
    case 0x9: *lres = r1; if (flags) update_nz(c, *lres); return 1;
    case 0xa: *lres = r0 | r1; if (flags) update_nz(c, *lres); return 1;
    case 0xb: *lres = r0 & r1; if (flags) update_nz(c, *lres); return 1;
    case 0xc: *lres = r0 & r1; if (flags) update_nz(c, *lres); return 0;
    case 0xd: WR(r2, r0); return 0;
    default: return 0; /* MAME logs and ignores illegal ALU ops */
    }
}

static void exec_remaining(unsp_t *c, uint16_t op)
{
    const int op0 = (op >> 12) & 15, opa = (op >> 9) & 7, op1 = (op >> 6) & 7;
    const int opn = (op >> 3) & 7, opb = op & 7;
    const int lower_op = (op1 << 4) | op0;
    uint32_t lres = 0, r2 = 0;
    uint16_t r0, r1 = 0;

    if (lower_op == 0x2d) { /* push */
        int n = opn, reg = opa;
        c->cycles += 4 + 2 * n;
        while (n--) push(c, R(reg--), opb);
        return;
    }
    if (lower_op == 0x29) {
        if (op == 0x9a98) { /* reti */
            c->cycles += 8;
            if (c->ine) set_fr(c, pop(c, REG_SP));
            R(REG_SR) = pop(c, REG_SP);
            R(REG_PC) = pop(c, REG_SP);
            if (c->in_fiq) c->in_fiq = 0;
            else if (c->in_irq) c->in_irq = 0;
            return;
        }
        int n = opn, reg = opa; /* pop (also retf = pop sr, pc) */
        c->cycles += 4 + 2 * n;
        while (n--) { ++reg; R(reg) = pop(c, opb); }
        return;
    }

    r0 = R(opa);
    switch (op1) {
    case 0: /* [bp+imm6] */
        c->cycles += 6;
        r2 = (uint16_t)(R(REG_BP) + (op & 0x3f));
        if (op0 != 0xd) r1 = RD(r2);
        break;
    case 1: /* imm6 */
        c->cycles += 2;
        r1 = op & 0x3f;
        break;
    case 3: { /* indirect */
        c->cycles += (opa == 7) ? 7 : 6;
        const int mode = opn & 3;
        if (opn & 4) { /* ds:[r] */
            if (mode == 3) {
                R(opb) = (uint16_t)(R(opb) + 1);
                if (R(opb) == 0) R(REG_SR) += 0x0400;
            }
            r2 = lreg_i(c, opb);
            if (op0 != 0xd) r1 = RD(r2);
            if (mode == 1) {
                R(opb) = (uint16_t)(R(opb) - 1);
                if (R(opb) == 0xffff) R(REG_SR) -= 0x0400;
            } else if (mode == 2) {
                R(opb) = (uint16_t)(R(opb) + 1);
                if (R(opb) == 0) R(REG_SR) += 0x0400;
            }
        } else {
            if (mode == 3) R(opb) = (uint16_t)(R(opb) + 1);
            r2 = R(opb);
            if (op0 != 0xd) r1 = RD(r2);
            if (mode == 1) R(opb) = (uint16_t)(R(opb) - 1);
            else if (mode == 2) R(opb) = (uint16_t)(R(opb) + 1);
        }
        break;
    }
    case 4:
        switch (opn) {
        case 0: /* r */
            c->cycles += (opa == 7) ? 5 : 3;
            r1 = R(opb);
            break;
        case 1: /* imm16 */
            c->cycles += (opa == 7) ? 5 : 4;
            r0 = R(opb);
            r1 = fetch(c);
            break;
        case 2: /* [imm16] */
            c->cycles += (opa == 7) ? 8 : 7;
            r0 = R(opb);
            r2 = fetch(c);
            if (op0 != 0xd) r1 = RD(r2);
            break;
        case 3: /* [imm16] = ... */
            c->cycles += (opa == 7) ? 8 : 7;
            r1 = r0;
            r0 = R(opb);
            r2 = fetch(c);
            break;
        default: { /* asr */
            c->cycles += (opa == 7) ? 5 : 3;
            uint32_t shift = ((uint32_t)R(opb) << 4) | c->sb;
            if (shift & 0x80000) shift |= 0xf00000;
            shift >>= (opn - 3);
            c->sb = shift & 0x0f;
            r1 = (uint16_t)(shift >> 4);
            break;
        }
        }
        break;
    case 5: /* lsl / lsr */
        c->cycles += (opa == 7) ? 5 : 3;
        if (opn & 4) {
            const uint32_t shift = (((uint32_t)R(opb) << 4) | c->sb) >> (opn - 3);
            c->sb = shift & 0x0f;
            r1 = (uint16_t)(shift >> 4);
        } else {
            const uint32_t shift = (((uint32_t)c->sb << 16) | R(opb)) << (opn + 1);
            c->sb = (shift >> 16) & 0x0f;
            r1 = (uint16_t)shift;
        }
        break;
    case 6: { /* rol / ror */
        c->cycles += (opa == 7) ? 5 : 3;
        uint32_t shift = ((((uint32_t)c->sb << 16) | R(opb)) << 4) | c->sb;
        if (opn & 4) {
            shift >>= (opn - 3);
            c->sb = shift & 0x0f;
        } else {
            shift <<= (opn + 1);
            c->sb = (shift >> 20) & 0x0f;
        }
        r1 = (uint16_t)(shift >> 4);
        break;
    }
    case 7: /* [imm6] */
        c->cycles += (opa == 7) ? 6 : 5;
        r2 = op & 0x3f;
        r1 = RD(r2);
        break;
    default:
        break;
    }

    if (alu(c, op0, &lres, r0, r1, r2, opa != 7)) {
        if (op1 == 4 && opn == 3) WR(r2, (uint16_t)lres);
        else R(opa) = (uint16_t)lres;
    }
}

uint16_t unsp_step(unsp_t *c)
{
    const uint16_t op = fetch(c);
    const int op0 = (op >> 12) & 15, opa = (op >> 9) & 7, op1 = (op >> 6) & 7;

    if (op0 == 0xf) exec_fxxx(c, op);
    else if (opa == 7 && op1 < 2) exec_jump(c, op);
    else if (op0 == 0xe) bad(c, op); /* bit ops / shifts are ISA 1.2+ */
    else exec_remaining(c, op);

    if (op != 0x9a98) check_irqs(c);
    return op;
}
