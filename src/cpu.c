#include "nes.h"

/* ---- helpers ---- */

static inline void tick(NES *n) { nes_tick(n); }

static inline uint8_t rd(NES *n, uint16_t a) {
    tick(n);
    return bus_read(n, a);
}

static inline void wr(NES *n, uint16_t a, uint8_t v) {
    tick(n);
    bus_write(n, a, v);
}

static inline void set_zn(CPU *c, uint8_t v) {
    c->status = (c->status & ~(FLAG_Z | FLAG_N))
              | (v == 0 ? FLAG_Z : 0)
              | (v & 0x80);
}

static inline void push8(NES *n, uint8_t v) {
    wr(n, 0x0100 | n->cpu.sp--, v);
}

static inline uint8_t pull8(NES *n) {
    n->cpu.sp++;
    return rd(n, 0x0100 | n->cpu.sp);
}

/* ---- addressing modes (each ticks the bus appropriately) ---- */

static uint16_t am_zpg(NES *n)   { return rd(n, n->cpu.pc++); }

static uint16_t am_zpx(NES *n) {
    uint8_t b = rd(n, n->cpu.pc++);
    rd(n, b); /* dummy */
    return (b + n->cpu.x) & 0xFF;
}

static uint16_t am_zpy(NES *n) {
    uint8_t b = rd(n, n->cpu.pc++);
    rd(n, b);
    return (b + n->cpu.y) & 0xFF;
}

static uint16_t am_abs(NES *n) {
    uint8_t lo = rd(n, n->cpu.pc++);
    uint8_t hi = rd(n, n->cpu.pc++);
    return lo | (hi << 8);
}

static uint16_t am_abx_r(NES *n) {
    uint8_t lo = rd(n, n->cpu.pc++);
    uint8_t hi = rd(n, n->cpu.pc++);
    uint16_t a = (lo | (hi << 8)) + n->cpu.x;
    if ((a & 0xFF00) != ((uint16_t)hi << 8))
        rd(n, (hi << 8) | ((lo + n->cpu.x) & 0xFF));
    return a;
}

static uint16_t am_abx_w(NES *n) {
    uint8_t lo = rd(n, n->cpu.pc++);
    uint8_t hi = rd(n, n->cpu.pc++);
    rd(n, (hi << 8) | ((lo + n->cpu.x) & 0xFF));
    return (lo | (hi << 8)) + n->cpu.x;
}

static uint16_t am_aby_r(NES *n) {
    uint8_t lo = rd(n, n->cpu.pc++);
    uint8_t hi = rd(n, n->cpu.pc++);
    uint16_t a = (lo | (hi << 8)) + n->cpu.y;
    if ((a & 0xFF00) != ((uint16_t)hi << 8))
        rd(n, (hi << 8) | ((lo + n->cpu.y) & 0xFF));
    return a;
}

static uint16_t am_aby_w(NES *n) {
    uint8_t lo = rd(n, n->cpu.pc++);
    uint8_t hi = rd(n, n->cpu.pc++);
    rd(n, (hi << 8) | ((lo + n->cpu.y) & 0xFF));
    return (lo | (hi << 8)) + n->cpu.y;
}

static uint16_t am_izx(NES *n) {
    uint8_t p = rd(n, n->cpu.pc++);
    rd(n, p);
    p += n->cpu.x;
    uint8_t lo = rd(n, p & 0xFF);
    uint8_t hi = rd(n, (p + 1) & 0xFF);
    return lo | (hi << 8);
}

static uint16_t am_izy_r(NES *n) {
    uint8_t p = rd(n, n->cpu.pc++);
    uint8_t lo = rd(n, p & 0xFF);
    uint8_t hi = rd(n, (p + 1) & 0xFF);
    uint16_t a = (lo | (hi << 8)) + n->cpu.y;
    if ((a & 0xFF00) != ((uint16_t)hi << 8))
        rd(n, (hi << 8) | ((lo + n->cpu.y) & 0xFF));
    return a;
}

static uint16_t am_izy_w(NES *n) {
    uint8_t p = rd(n, n->cpu.pc++);
    uint8_t lo = rd(n, p & 0xFF);
    uint8_t hi = rd(n, (p + 1) & 0xFF);
    rd(n, (hi << 8) | ((lo + n->cpu.y) & 0xFF));
    return (lo | (hi << 8)) + n->cpu.y;
}

/* ---- operations ---- */

static void do_branch(NES *n, bool cond) {
    int8_t off = (int8_t)rd(n, n->cpu.pc++);
    if (cond) {
        rd(n, n->cpu.pc);
        uint16_t np = n->cpu.pc + off;
        if ((np & 0xFF00) != (n->cpu.pc & 0xFF00))
            rd(n, (n->cpu.pc & 0xFF00) | (np & 0x00FF));
        n->cpu.pc = np;
    }
}

static void do_adc(NES *n, uint8_t v) {
    CPU *c = &n->cpu;
    uint16_t sum = c->a + v + (c->status & FLAG_C);
    c->status &= ~(FLAG_C | FLAG_Z | FLAG_V | FLAG_N);
    if (sum > 0xFF) c->status |= FLAG_C;
    if (!((c->a ^ v) & 0x80) && ((c->a ^ sum) & 0x80)) c->status |= FLAG_V;
    c->a = sum & 0xFF;
    if (c->a == 0) c->status |= FLAG_Z;
    c->status |= (c->a & 0x80);
}

static void do_sbc(NES *n, uint8_t v) {
    do_adc(n, ~v);
}

static void do_cmp(CPU *c, uint8_t a, uint8_t b) {
    uint16_t r = a - b;
    c->status &= ~(FLAG_C | FLAG_Z | FLAG_N);
    if (a >= b) c->status |= FLAG_C;
    if (a == b) c->status |= FLAG_Z;
    c->status |= (r & 0x80);
}

/* ---- interrupt helpers ---- */

static void do_irq_nmi(NES *n, uint16_t vec, bool brk) {
    if (brk) n->cpu.pc++;
    rd(n, n->cpu.pc); /* dummy */
    if (!brk) rd(n, n->cpu.pc); /* extra dummy for hw interrupt */
    push8(n, n->cpu.pc >> 8);
    push8(n, n->cpu.pc & 0xFF);
    push8(n, (n->cpu.status | FLAG_U | (brk ? FLAG_B : 0)) & (brk ? 0xFF : ~FLAG_B));
    n->cpu.status |= FLAG_I;
    uint8_t lo = rd(n, vec);
    uint8_t hi = rd(n, vec + 1);
    n->cpu.pc = lo | (hi << 8);
}

/* ---- main step ---- */

void cpu_init(NES *n) {
    memset(&n->cpu, 0, sizeof(CPU));
    n->cpu.status = FLAG_I | FLAG_U;
    n->cpu.sp = 0xFD;
}

void cpu_reset(NES *n) {
    CPU *c = &n->cpu;
    c->sp -= 3;
    c->status |= FLAG_I;
    uint8_t lo = bus_read(n, 0xFFFC);
    uint8_t hi = bus_read(n, 0xFFFD);
    c->pc = lo | (hi << 8);
    c->dma_active = false;
    c->total_cycles = 7;
}

void cpu_step(NES *n) {
    CPU *c = &n->cpu;

    /* NMI */
    if (c->nmi_pending) {
        c->nmi_pending = false;
        do_irq_nmi(n, 0xFFFA, false);
        return;
    }

    /* IRQ */
    if (c->irq_line && !(c->status & FLAG_I)) {
        do_irq_nmi(n, 0xFFFE, false);
        return;
    }

    uint8_t op = rd(n, c->pc++);
    uint16_t addr;
    uint8_t  val, tmp;

    switch (op) {

    /* ===== BRK ===== */
    case 0x00: do_irq_nmi(n, 0xFFFE, true); break;

    /* ===== ORA ===== */
    case 0x09: c->a |= rd(n, c->pc++); set_zn(c, c->a); break;
    case 0x05: c->a |= rd(n, am_zpg(n)); set_zn(c, c->a); break;
    case 0x15: c->a |= rd(n, am_zpx(n)); set_zn(c, c->a); break;
    case 0x0D: c->a |= rd(n, am_abs(n)); set_zn(c, c->a); break;
    case 0x1D: c->a |= rd(n, am_abx_r(n)); set_zn(c, c->a); break;
    case 0x19: c->a |= rd(n, am_aby_r(n)); set_zn(c, c->a); break;
    case 0x01: c->a |= rd(n, am_izx(n)); set_zn(c, c->a); break;
    case 0x11: c->a |= rd(n, am_izy_r(n)); set_zn(c, c->a); break;

    /* ===== AND ===== */
    case 0x29: c->a &= rd(n, c->pc++); set_zn(c, c->a); break;
    case 0x25: c->a &= rd(n, am_zpg(n)); set_zn(c, c->a); break;
    case 0x35: c->a &= rd(n, am_zpx(n)); set_zn(c, c->a); break;
    case 0x2D: c->a &= rd(n, am_abs(n)); set_zn(c, c->a); break;
    case 0x3D: c->a &= rd(n, am_abx_r(n)); set_zn(c, c->a); break;
    case 0x39: c->a &= rd(n, am_aby_r(n)); set_zn(c, c->a); break;
    case 0x21: c->a &= rd(n, am_izx(n)); set_zn(c, c->a); break;
    case 0x31: c->a &= rd(n, am_izy_r(n)); set_zn(c, c->a); break;

    /* ===== EOR ===== */
    case 0x49: c->a ^= rd(n, c->pc++); set_zn(c, c->a); break;
    case 0x45: c->a ^= rd(n, am_zpg(n)); set_zn(c, c->a); break;
    case 0x55: c->a ^= rd(n, am_zpx(n)); set_zn(c, c->a); break;
    case 0x4D: c->a ^= rd(n, am_abs(n)); set_zn(c, c->a); break;
    case 0x5D: c->a ^= rd(n, am_abx_r(n)); set_zn(c, c->a); break;
    case 0x59: c->a ^= rd(n, am_aby_r(n)); set_zn(c, c->a); break;
    case 0x41: c->a ^= rd(n, am_izx(n)); set_zn(c, c->a); break;
    case 0x51: c->a ^= rd(n, am_izy_r(n)); set_zn(c, c->a); break;

    /* ===== ADC ===== */
    case 0x69: do_adc(n, rd(n, c->pc++)); break;
    case 0x65: do_adc(n, rd(n, am_zpg(n))); break;
    case 0x75: do_adc(n, rd(n, am_zpx(n))); break;
    case 0x6D: do_adc(n, rd(n, am_abs(n))); break;
    case 0x7D: do_adc(n, rd(n, am_abx_r(n))); break;
    case 0x79: do_adc(n, rd(n, am_aby_r(n))); break;
    case 0x61: do_adc(n, rd(n, am_izx(n))); break;
    case 0x71: do_adc(n, rd(n, am_izy_r(n))); break;

    /* ===== SBC ===== */
    case 0xE9: case 0xEB: do_sbc(n, rd(n, c->pc++)); break;
    case 0xE5: do_sbc(n, rd(n, am_zpg(n))); break;
    case 0xF5: do_sbc(n, rd(n, am_zpx(n))); break;
    case 0xED: do_sbc(n, rd(n, am_abs(n))); break;
    case 0xFD: do_sbc(n, rd(n, am_abx_r(n))); break;
    case 0xF9: do_sbc(n, rd(n, am_aby_r(n))); break;
    case 0xE1: do_sbc(n, rd(n, am_izx(n))); break;
    case 0xF1: do_sbc(n, rd(n, am_izy_r(n))); break;

    /* ===== CMP ===== */
    case 0xC9: do_cmp(c, c->a, rd(n, c->pc++)); break;
    case 0xC5: do_cmp(c, c->a, rd(n, am_zpg(n))); break;
    case 0xD5: do_cmp(c, c->a, rd(n, am_zpx(n))); break;
    case 0xCD: do_cmp(c, c->a, rd(n, am_abs(n))); break;
    case 0xDD: do_cmp(c, c->a, rd(n, am_abx_r(n))); break;
    case 0xD9: do_cmp(c, c->a, rd(n, am_aby_r(n))); break;
    case 0xC1: do_cmp(c, c->a, rd(n, am_izx(n))); break;
    case 0xD1: do_cmp(c, c->a, rd(n, am_izy_r(n))); break;

    /* ===== CPX ===== */
    case 0xE0: do_cmp(c, c->x, rd(n, c->pc++)); break;
    case 0xE4: do_cmp(c, c->x, rd(n, am_zpg(n))); break;
    case 0xEC: do_cmp(c, c->x, rd(n, am_abs(n))); break;

    /* ===== CPY ===== */
    case 0xC0: do_cmp(c, c->y, rd(n, c->pc++)); break;
    case 0xC4: do_cmp(c, c->y, rd(n, am_zpg(n))); break;
    case 0xCC: do_cmp(c, c->y, rd(n, am_abs(n))); break;

    /* ===== LDA ===== */
    case 0xA9: c->a = rd(n, c->pc++); set_zn(c, c->a); break;
    case 0xA5: c->a = rd(n, am_zpg(n)); set_zn(c, c->a); break;
    case 0xB5: c->a = rd(n, am_zpx(n)); set_zn(c, c->a); break;
    case 0xAD: c->a = rd(n, am_abs(n)); set_zn(c, c->a); break;
    case 0xBD: c->a = rd(n, am_abx_r(n)); set_zn(c, c->a); break;
    case 0xB9: c->a = rd(n, am_aby_r(n)); set_zn(c, c->a); break;
    case 0xA1: c->a = rd(n, am_izx(n)); set_zn(c, c->a); break;
    case 0xB1: c->a = rd(n, am_izy_r(n)); set_zn(c, c->a); break;

    /* ===== LDX ===== */
    case 0xA2: c->x = rd(n, c->pc++); set_zn(c, c->x); break;
    case 0xA6: c->x = rd(n, am_zpg(n)); set_zn(c, c->x); break;
    case 0xB6: c->x = rd(n, am_zpy(n)); set_zn(c, c->x); break;
    case 0xAE: c->x = rd(n, am_abs(n)); set_zn(c, c->x); break;
    case 0xBE: c->x = rd(n, am_aby_r(n)); set_zn(c, c->x); break;

    /* ===== LDY ===== */
    case 0xA0: c->y = rd(n, c->pc++); set_zn(c, c->y); break;
    case 0xA4: c->y = rd(n, am_zpg(n)); set_zn(c, c->y); break;
    case 0xB4: c->y = rd(n, am_zpx(n)); set_zn(c, c->y); break;
    case 0xAC: c->y = rd(n, am_abs(n)); set_zn(c, c->y); break;
    case 0xBC: c->y = rd(n, am_abx_r(n)); set_zn(c, c->y); break;

    /* ===== STA ===== */
    case 0x85: wr(n, am_zpg(n), c->a); break;
    case 0x95: wr(n, am_zpx(n), c->a); break;
    case 0x8D: wr(n, am_abs(n), c->a); break;
    case 0x9D: wr(n, am_abx_w(n), c->a); break;
    case 0x99: wr(n, am_aby_w(n), c->a); break;
    case 0x81: wr(n, am_izx(n), c->a); break;
    case 0x91: wr(n, am_izy_w(n), c->a); break;

    /* ===== STX ===== */
    case 0x86: wr(n, am_zpg(n), c->x); break;
    case 0x96: wr(n, am_zpy(n), c->x); break;
    case 0x8E: wr(n, am_abs(n), c->x); break;

    /* ===== STY ===== */
    case 0x84: wr(n, am_zpg(n), c->y); break;
    case 0x94: wr(n, am_zpx(n), c->y); break;
    case 0x8C: wr(n, am_abs(n), c->y); break;

    /* ===== ASL ===== */
    case 0x0A: /* acc */
        rd(n, c->pc);
        c->status = (c->status & ~FLAG_C) | ((c->a >> 7) & 1);
        c->a <<= 1;
        set_zn(c, c->a);
        break;
    case 0x06: addr = am_zpg(n); goto asl_mem;
    case 0x16: addr = am_zpx(n); goto asl_mem;
    case 0x0E: addr = am_abs(n); goto asl_mem;
    case 0x1E: addr = am_abx_w(n);
    asl_mem:
        val = rd(n, addr); wr(n, addr, val);
        c->status = (c->status & ~FLAG_C) | ((val >> 7) & 1);
        val <<= 1; set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== LSR ===== */
    case 0x4A:
        rd(n, c->pc);
        c->status = (c->status & ~FLAG_C) | (c->a & 1);
        c->a >>= 1;
        set_zn(c, c->a);
        break;
    case 0x46: addr = am_zpg(n); goto lsr_mem;
    case 0x56: addr = am_zpx(n); goto lsr_mem;
    case 0x4E: addr = am_abs(n); goto lsr_mem;
    case 0x5E: addr = am_abx_w(n);
    lsr_mem:
        val = rd(n, addr); wr(n, addr, val);
        c->status = (c->status & ~FLAG_C) | (val & 1);
        val >>= 1; set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== ROL ===== */
    case 0x2A:
        rd(n, c->pc);
        tmp = (c->a >> 7) & 1;
        c->a = (c->a << 1) | (c->status & FLAG_C);
        c->status = (c->status & ~FLAG_C) | tmp;
        set_zn(c, c->a);
        break;
    case 0x26: addr = am_zpg(n); goto rol_mem;
    case 0x36: addr = am_zpx(n); goto rol_mem;
    case 0x2E: addr = am_abs(n); goto rol_mem;
    case 0x3E: addr = am_abx_w(n);
    rol_mem:
        val = rd(n, addr); wr(n, addr, val);
        tmp = (val >> 7) & 1;
        val = (val << 1) | (c->status & FLAG_C);
        c->status = (c->status & ~FLAG_C) | tmp;
        set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== ROR ===== */
    case 0x6A:
        rd(n, c->pc);
        tmp = c->a & 1;
        c->a = (c->a >> 1) | ((c->status & FLAG_C) << 7);
        c->status = (c->status & ~FLAG_C) | tmp;
        set_zn(c, c->a);
        break;
    case 0x66: addr = am_zpg(n); goto ror_mem;
    case 0x76: addr = am_zpx(n); goto ror_mem;
    case 0x6E: addr = am_abs(n); goto ror_mem;
    case 0x7E: addr = am_abx_w(n);
    ror_mem:
        val = rd(n, addr); wr(n, addr, val);
        tmp = val & 1;
        val = (val >> 1) | ((c->status & FLAG_C) << 7);
        c->status = (c->status & ~FLAG_C) | tmp;
        set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== INC ===== */
    case 0xE6: addr = am_zpg(n); goto inc_mem;
    case 0xF6: addr = am_zpx(n); goto inc_mem;
    case 0xEE: addr = am_abs(n); goto inc_mem;
    case 0xFE: addr = am_abx_w(n);
    inc_mem:
        val = rd(n, addr); wr(n, addr, val);
        val++; set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== DEC ===== */
    case 0xC6: addr = am_zpg(n); goto dec_mem;
    case 0xD6: addr = am_zpx(n); goto dec_mem;
    case 0xCE: addr = am_abs(n); goto dec_mem;
    case 0xDE: addr = am_abx_w(n);
    dec_mem:
        val = rd(n, addr); wr(n, addr, val);
        val--; set_zn(c, val);
        wr(n, addr, val);
        break;

    /* ===== INX/INY/DEX/DEY ===== */
    case 0xE8: rd(n, c->pc); c->x++; set_zn(c, c->x); break;
    case 0xC8: rd(n, c->pc); c->y++; set_zn(c, c->y); break;
    case 0xCA: rd(n, c->pc); c->x--; set_zn(c, c->x); break;
    case 0x88: rd(n, c->pc); c->y--; set_zn(c, c->y); break;

    /* ===== BIT ===== */
    case 0x24:
        val = rd(n, am_zpg(n));
        c->status = (c->status & ~(FLAG_Z | FLAG_V | FLAG_N))
                   | ((c->a & val) == 0 ? FLAG_Z : 0)
                   | (val & (FLAG_V | FLAG_N));
        break;
    case 0x2C:
        val = rd(n, am_abs(n));
        c->status = (c->status & ~(FLAG_Z | FLAG_V | FLAG_N))
                   | ((c->a & val) == 0 ? FLAG_Z : 0)
                   | (val & (FLAG_V | FLAG_N));
        break;

    /* ===== Branches ===== */
    case 0x10: do_branch(n, !(c->status & FLAG_N)); break; /* BPL */
    case 0x30: do_branch(n,  (c->status & FLAG_N)); break; /* BMI */
    case 0x50: do_branch(n, !(c->status & FLAG_V)); break; /* BVC */
    case 0x70: do_branch(n,  (c->status & FLAG_V)); break; /* BVS */
    case 0x90: do_branch(n, !(c->status & FLAG_C)); break; /* BCC */
    case 0xB0: do_branch(n,  (c->status & FLAG_C)); break; /* BCS */
    case 0xD0: do_branch(n, !(c->status & FLAG_Z)); break; /* BNE */
    case 0xF0: do_branch(n,  (c->status & FLAG_Z)); break; /* BEQ */

    /* ===== JMP ===== */
    case 0x4C: { /* absolute */
        uint8_t lo = rd(n, c->pc++);
        uint8_t hi = rd(n, c->pc);
        c->pc = lo | (hi << 8);
    } break;
    case 0x6C: { /* indirect - with page wrap bug */
        uint8_t plo = rd(n, c->pc++);
        uint8_t phi = rd(n, c->pc);
        uint16_t ptr = plo | (phi << 8);
        uint8_t lo = rd(n, ptr);
        uint8_t hi = rd(n, (ptr & 0xFF00) | ((ptr + 1) & 0xFF));
        c->pc = lo | (hi << 8);
    } break;

    /* ===== JSR ===== */
    case 0x20: {
        uint8_t lo = rd(n, c->pc++);
        rd(n, 0x0100 | c->sp); /* internal */
        push8(n, c->pc >> 8);
        push8(n, c->pc & 0xFF);
        uint8_t hi = rd(n, c->pc);
        c->pc = lo | (hi << 8);
    } break;

    /* ===== RTS ===== */
    case 0x60:
        rd(n, c->pc); /* dummy */
        rd(n, 0x0100 | c->sp); /* dummy stack */
        {
            uint8_t lo = pull8(n);
            uint8_t hi = pull8(n);
            c->pc = (lo | (hi << 8)) + 1;
        }
        rd(n, c->pc); /* dummy */
        break;

    /* ===== RTI ===== */
    case 0x40:
        rd(n, c->pc);
        rd(n, 0x0100 | c->sp);
        c->status = (pull8(n) & ~FLAG_B) | FLAG_U;
        {
            uint8_t lo = pull8(n);
            uint8_t hi = pull8(n);
            c->pc = lo | (hi << 8);
        }
        break;

    /* ===== Stack ===== */
    case 0x48: /* PHA */ rd(n, c->pc); push8(n, c->a); break;
    case 0x08: /* PHP */ rd(n, c->pc); push8(n, c->status | FLAG_B | FLAG_U); break;
    case 0x68: /* PLA */
        rd(n, c->pc);
        rd(n, 0x0100 | c->sp);
        c->a = pull8(n);
        set_zn(c, c->a);
        break;
    case 0x28: /* PLP */
        rd(n, c->pc);
        rd(n, 0x0100 | c->sp);
        c->status = (pull8(n) & ~FLAG_B) | FLAG_U;
        break;

    /* ===== Transfer ===== */
    case 0xAA: rd(n, c->pc); c->x = c->a; set_zn(c, c->x); break; /* TAX */
    case 0xA8: rd(n, c->pc); c->y = c->a; set_zn(c, c->y); break; /* TAY */
    case 0xBA: rd(n, c->pc); c->x = c->sp; set_zn(c, c->x); break; /* TSX */
    case 0x8A: rd(n, c->pc); c->a = c->x; set_zn(c, c->a); break; /* TXA */
    case 0x9A: rd(n, c->pc); c->sp = c->x; break; /* TXS */
    case 0x98: rd(n, c->pc); c->a = c->y; set_zn(c, c->a); break; /* TYA */

    /* ===== Flags ===== */
    case 0x18: rd(n, c->pc); c->status &= ~FLAG_C; break; /* CLC */
    case 0x38: rd(n, c->pc); c->status |=  FLAG_C; break; /* SEC */
    case 0x58: rd(n, c->pc); c->status &= ~FLAG_I; break; /* CLI */
    case 0x78: rd(n, c->pc); c->status |=  FLAG_I; break; /* SEI */
    case 0xB8: rd(n, c->pc); c->status &= ~FLAG_V; break; /* CLV */
    case 0xD8: rd(n, c->pc); c->status &= ~FLAG_D; break; /* CLD */
    case 0xF8: rd(n, c->pc); c->status |=  FLAG_D; break; /* SED */

    /* ===== NOP ===== */
    case 0xEA: rd(n, c->pc); break;

    /* ===== Unofficial NOPs (various sizes) ===== */
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        rd(n, c->pc); break; /* 1-byte implied NOP */
    case 0x04: case 0x44: case 0x64:
        rd(n, c->pc++); rd(n, 0); break; /* 2-byte ZPG NOP */
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
        rd(n, c->pc++); rd(n, 0); break; /* 2-byte ZPX NOP */
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
        rd(n, c->pc++); break; /* 2-byte IMM NOP */
    case 0x0C:
        am_abs(n); break; /* 3-byte ABS NOP */
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        am_abx_r(n); break; /* 3-byte ABX NOP */

    /* ===== Unofficial: LAX (LDA + LDX) ===== */
    case 0xA7: c->a = c->x = rd(n, am_zpg(n)); set_zn(c, c->a); break;
    case 0xB7: c->a = c->x = rd(n, am_zpy(n)); set_zn(c, c->a); break;
    case 0xAF: c->a = c->x = rd(n, am_abs(n)); set_zn(c, c->a); break;
    case 0xBF: c->a = c->x = rd(n, am_aby_r(n)); set_zn(c, c->a); break;
    case 0xA3: c->a = c->x = rd(n, am_izx(n)); set_zn(c, c->a); break;
    case 0xB3: c->a = c->x = rd(n, am_izy_r(n)); set_zn(c, c->a); break;

    /* ===== Unofficial: SAX (A & X -> mem) ===== */
    case 0x87: wr(n, am_zpg(n), c->a & c->x); break;
    case 0x97: wr(n, am_zpy(n), c->a & c->x); break;
    case 0x8F: wr(n, am_abs(n), c->a & c->x); break;
    case 0x83: wr(n, am_izx(n), c->a & c->x); break;

    /* ===== Unofficial: DCP (DEC + CMP) ===== */
    case 0xC7: addr = am_zpg(n); goto dcp;
    case 0xD7: addr = am_zpx(n); goto dcp;
    case 0xCF: addr = am_abs(n); goto dcp;
    case 0xDF: addr = am_abx_w(n); goto dcp;
    case 0xDB: addr = am_aby_w(n); goto dcp;
    case 0xC3: addr = am_izx(n); goto dcp;
    case 0xD3: addr = am_izy_w(n);
    dcp:
        val = rd(n, addr); wr(n, addr, val);
        val--; wr(n, addr, val);
        do_cmp(c, c->a, val);
        break;

    /* ===== Unofficial: ISB/ISC (INC + SBC) ===== */
    case 0xE7: addr = am_zpg(n); goto isb;
    case 0xF7: addr = am_zpx(n); goto isb;
    case 0xEF: addr = am_abs(n); goto isb;
    case 0xFF: addr = am_abx_w(n); goto isb;
    case 0xFB: addr = am_aby_w(n); goto isb;
    case 0xE3: addr = am_izx(n); goto isb;
    case 0xF3: addr = am_izy_w(n);
    isb:
        val = rd(n, addr); wr(n, addr, val);
        val++; wr(n, addr, val);
        do_sbc(n, val);
        break;

    /* ===== Unofficial: SLO (ASL + ORA) ===== */
    case 0x07: addr = am_zpg(n); goto slo;
    case 0x17: addr = am_zpx(n); goto slo;
    case 0x0F: addr = am_abs(n); goto slo;
    case 0x1F: addr = am_abx_w(n); goto slo;
    case 0x1B: addr = am_aby_w(n); goto slo;
    case 0x03: addr = am_izx(n); goto slo;
    case 0x13: addr = am_izy_w(n);
    slo:
        val = rd(n, addr); wr(n, addr, val);
        c->status = (c->status & ~FLAG_C) | ((val >> 7) & 1);
        val <<= 1; wr(n, addr, val);
        c->a |= val; set_zn(c, c->a);
        break;

    /* ===== Unofficial: RLA (ROL + AND) ===== */
    case 0x27: addr = am_zpg(n); goto rla;
    case 0x37: addr = am_zpx(n); goto rla;
    case 0x2F: addr = am_abs(n); goto rla;
    case 0x3F: addr = am_abx_w(n); goto rla;
    case 0x3B: addr = am_aby_w(n); goto rla;
    case 0x23: addr = am_izx(n); goto rla;
    case 0x33: addr = am_izy_w(n);
    rla:
        val = rd(n, addr); wr(n, addr, val);
        tmp = (val >> 7) & 1;
        val = (val << 1) | (c->status & FLAG_C);
        c->status = (c->status & ~FLAG_C) | tmp;
        wr(n, addr, val);
        c->a &= val; set_zn(c, c->a);
        break;

    /* ===== Unofficial: SRE (LSR + EOR) ===== */
    case 0x47: addr = am_zpg(n); goto sre;
    case 0x57: addr = am_zpx(n); goto sre;
    case 0x4F: addr = am_abs(n); goto sre;
    case 0x5F: addr = am_abx_w(n); goto sre;
    case 0x5B: addr = am_aby_w(n); goto sre;
    case 0x43: addr = am_izx(n); goto sre;
    case 0x53: addr = am_izy_w(n);
    sre:
        val = rd(n, addr); wr(n, addr, val);
        c->status = (c->status & ~FLAG_C) | (val & 1);
        val >>= 1; wr(n, addr, val);
        c->a ^= val; set_zn(c, c->a);
        break;

    /* ===== Unofficial: RRA (ROR + ADC) ===== */
    case 0x67: addr = am_zpg(n); goto rra;
    case 0x77: addr = am_zpx(n); goto rra;
    case 0x6F: addr = am_abs(n); goto rra;
    case 0x7F: addr = am_abx_w(n); goto rra;
    case 0x7B: addr = am_aby_w(n); goto rra;
    case 0x63: addr = am_izx(n); goto rra;
    case 0x73: addr = am_izy_w(n);
    rra:
        val = rd(n, addr); wr(n, addr, val);
        tmp = val & 1;
        val = (val >> 1) | ((c->status & FLAG_C) << 7);
        c->status = (c->status & ~FLAG_C) | tmp;
        wr(n, addr, val);
        do_adc(n, val);
        break;

    /* ===== Unofficial: ANC ===== */
    case 0x0B: case 0x2B:
        c->a &= rd(n, c->pc++);
        set_zn(c, c->a);
        c->status = (c->status & ~FLAG_C) | ((c->a >> 7) & 1);
        break;

    /* ===== Unofficial: ALR (AND + LSR) ===== */
    case 0x4B:
        c->a &= rd(n, c->pc++);
        c->status = (c->status & ~FLAG_C) | (c->a & 1);
        c->a >>= 1;
        set_zn(c, c->a);
        break;

    /* ===== Unofficial: ARR (AND + ROR, weird flags) ===== */
    case 0x6B:
        c->a &= rd(n, c->pc++);
        c->a = (c->a >> 1) | ((c->status & FLAG_C) << 7);
        set_zn(c, c->a);
        c->status = (c->status & ~(FLAG_C | FLAG_V))
                   | ((c->a >> 6) & 1)
                   | (((c->a >> 6) ^ (c->a >> 5)) & 1) << 6;
        break;

    /* ===== Unofficial: AXS (A & X - imm) ===== */
    case 0xCB: {
        uint8_t v = rd(n, c->pc++);
        uint16_t r = (c->a & c->x) - v;
        c->status = (c->status & ~FLAG_C) | (r < 0x100 ? FLAG_C : 0);
        c->x = r & 0xFF;
        set_zn(c, c->x);
    } break;

    /* ===== KIL/JAM ===== */
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        c->pc--;
        rd(n, c->pc);
        break;

    /* ===== Remaining unstable unofficial opcodes - treated as NOP ===== */
    default:
        rd(n, c->pc);
        break;
    }
}
