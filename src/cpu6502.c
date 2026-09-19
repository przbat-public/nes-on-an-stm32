/*
 * cpu6502.c — the emulated 6502 core.
 *
 * Structure:
 *   - the registers and the memory interface live here,
 *   - the addressing helpers below are used by the generated opcode
 *     dispatch in cpu_ops.h (produced by tools/gen_6502.py),
 *   - every instruction ends with CYCLES(n), which records how many
 *     master cycles it burned; cpu_run() uses that for frame pacing.
 *
 * The NES's 2A03 has no decimal mode, so the D flag is stored but never
 * changes arithmetic behaviour (as on real hardware).
 */
#include "cpu6502.h"

#ifdef NES_BUS_INLINE
#include "nesmem.h"     /* inlined fast paths for RAM and cartridge ROM */
#endif

cpu_t cpu;

/* cycles used by the instruction currently executing */
static int  cyc;
/* charged by the bus for things like OAM DMA: the CPU really does stop
 * for these cycles, so they must come out of the frame budget too */
int cpu_stall;
/* set to 1 by an addressing mode that crossed a page boundary */
static int  page;

#define CYCLES(n) (cyc = (n))

/* ------------------------------ memory ---------------------------- */

static inline uint8_t  rd(uint16_t a)            { return bus_read(a); }
static inline void     wr(uint16_t a, uint8_t v) { bus_write(a, v); }
static inline uint8_t  fetch8(void)              { return bus_read(cpu.pc++); }
static inline uint16_t fetch16(void)
{
    uint16_t lo = fetch8();
    return (uint16_t)(lo | (fetch8() << 8));
}

static inline void setzn(uint8_t v)
{
    cpu.p = (uint8_t)((cpu.p & ~(F_Z | F_N)) |
                      (v == 0 ? F_Z : 0) | (v & F_N));
}

/* ------------------------------ stack ----------------------------- */

static inline void push(uint8_t v)  { bus_write((uint16_t)(0x0100 + cpu.sp--), v); }
static inline uint8_t pull(void)    { return bus_read((uint16_t)(0x0100 + ++cpu.sp)); }
static inline void push16(uint16_t v)
{
    push((uint8_t)(v >> 8));
    push((uint8_t)(v & 0xFF));
}
static inline uint16_t pull16(void)
{
    uint16_t lo = pull();
    return (uint16_t)(lo | (pull() << 8));
}

/* --------------------------- addressing --------------------------- */
/* Each helper returns the effective address (or, for imm(), the address
 * of the operand byte) and updates cpu.pc as the instruction requires. */

static inline uint16_t imm(void) { return cpu.pc++; }
static inline uint16_t zp(void)  { return fetch8(); }
static inline uint16_t zpx(void) { return (uint16_t)((fetch8() + cpu.x) & 0xFF); }
static inline uint16_t zpy(void) { return (uint16_t)((fetch8() + cpu.y) & 0xFF); }
static inline uint16_t abs_(void) { return fetch16(); }

static inline uint16_t abx(void)
{
    uint16_t base = fetch16();
    uint16_t a = (uint16_t)(base + cpu.x);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

static inline uint16_t aby(void)
{
    uint16_t base = fetch16();
    uint16_t a = (uint16_t)(base + cpu.y);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

/* (indirect,X) — pointer read from zero page with wraparound */
static inline uint16_t inx(void)
{
    uint8_t p = (uint8_t)(fetch8() + cpu.x);
    uint8_t lo = bus_read(p);
    uint8_t hi = bus_read((uint8_t)(p + 1));
    return (uint16_t)(lo | (hi << 8));
}

/* (indirect),Y — zero-page pointer, then add Y */
static inline uint16_t iny(void)
{
    uint8_t p = fetch8();
    uint8_t lo = bus_read(p);
    uint8_t hi = bus_read((uint8_t)(p + 1));
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t a = (uint16_t)(base + cpu.y);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

/* branches: returns the cycle count directly */
static int branch(int take)
{
    int8_t off = (int8_t)fetch8();
    if (!take)
        return 2;
    uint16_t old = cpu.pc;
    cpu.pc = (uint16_t)(cpu.pc + off);
    return 3 + (((old ^ cpu.pc) & 0xFF00) ? 1 : 0);
}

/* ------------------------------- ALU ------------------------------ */

static void a_ora(uint8_t v) { cpu.a |= v;  setzn(cpu.a); }
static void a_and(uint8_t v) { cpu.a &= v;  setzn(cpu.a); }
static void a_eor(uint8_t v) { cpu.a ^= v;  setzn(cpu.a); }

static void a_adc(uint8_t v)
{
    unsigned carry = (cpu.p & F_C) ? 1u : 0u;
    unsigned r = (unsigned)cpu.a + v + carry;
    uint8_t  res = (uint8_t)r;

    cpu.p &= (uint8_t)~F_C;
    if (r > 0xFF) cpu.p |= F_C;
    /* signed overflow: both inputs same sign, result different */
    if (~(cpu.a ^ v) & (cpu.a ^ res) & 0x80) cpu.p |= F_V;
    cpu.a = res;
    setzn(res);
}

static void a_sbc(uint8_t v) { a_adc((uint8_t)~v); }

static void cmp_reg(uint8_t r, uint8_t v)
{
    unsigned t = (unsigned)r - v;
    cpu.p &= (uint8_t)~F_C;
    if (r >= v) cpu.p |= F_C;
    setzn((uint8_t)t);
}

static void a_cmp(uint8_t v) { cmp_reg(cpu.a, v); }
static void a_cpx(uint8_t v) { cmp_reg(cpu.x, v); }
static void a_cpy(uint8_t v) { cmp_reg(cpu.y, v); }

static uint8_t asl(uint8_t v)
{
    cpu.p = (uint8_t)((cpu.p & ~F_C) | ((v >> 7) & 1));
    uint8_t r = (uint8_t)(v << 1);
    setzn(r);
    return r;
}

static uint8_t lsr(uint8_t v)
{
    cpu.p = (uint8_t)((cpu.p & ~F_C) | (v & 1));
    uint8_t r = (uint8_t)(v >> 1);
    setzn(r);
    return r;
}

static uint8_t rol(uint8_t v)
{
    uint8_t c = (cpu.p & F_C) ? 1 : 0;
    cpu.p = (uint8_t)((cpu.p & ~F_C) | ((v >> 7) & 1));
    uint8_t r = (uint8_t)((v << 1) | c);
    setzn(r);
    return r;
}

static uint8_t ror(uint8_t v)
{
    uint8_t c = (cpu.p & F_C) ? 1 : 0;
    cpu.p = (uint8_t)((cpu.p & ~F_C) | (v & 1));
    uint8_t r = (uint8_t)((v >> 1) | (c << 7));
    setzn(r);
    return r;
}

static uint8_t inc8(uint8_t v) { v++; setzn(v); return v; }
static uint8_t dec8(uint8_t v) { v--; setzn(v); return v; }

/* --------------------- control-flow instructions ------------------ */

static void brk(void)
{
    push16((uint16_t)(cpu.pc + 1));
    push((uint8_t)(cpu.p | F_B | F_U));
    cpu.p |= F_I;
    cpu.pc = (uint16_t)(bus_read(0xFFFE) | (bus_read(0xFFFF) << 8));
}

static void rts(void)
{
    cpu.pc = (uint16_t)(pull16() + 1);
}

static void rti(void)
{
    cpu.p = (uint8_t)((pull() & ~F_B) | F_U);
    cpu.pc = pull16();
}

/* the famous page-boundary bug of the indirect jump */
static void jmp_ind(void)
{
    uint16_t a  = fetch16();
    uint16_t lo = bus_read(a);
    uint16_t hi = bus_read((uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF)));
    cpu.pc = (uint16_t)(lo | (hi << 8));
}

/* --------------------------- public API --------------------------- */

void cpu_reset(void)
{
    cpu.a = cpu.x = cpu.y = 0;
    cpu.sp = 0xFD;
    cpu.p  = F_I | F_U;
    cpu.pc = (uint16_t)(bus_read(0xFFFC) | (bus_read(0xFFFD) << 8));
    cpu.cycles = 0;
    cpu.instructions = 0;
    cyc = page = 0;
}

static void do_nmi(void)
{
    push16(cpu.pc);
    push((uint8_t)((cpu.p & ~F_B) | F_U));
    cpu.p |= F_I;
    cpu.pc = (uint16_t)(bus_read(0xFFFA) | (bus_read(0xFFFB) << 8));
    cpu.cycles += 7;
}

void cpu_nmi(void) { do_nmi(); }

bool cpu_irq(void)
{
    if (cpu.p & F_I) return false;   /* masked: the line stays asserted */
    push16(cpu.pc);
    push((uint8_t)((cpu.p & ~F_B) | F_U));
    cpu.p |= F_I;
    cpu.pc = (uint16_t)(bus_read(0xFFFE) | (bus_read(0xFFFF) << 8));
    cpu.cycles += 7;
    return true;
}

int cpu_step(void)
{
    cyc = 0;
    page = 0;
    cpu.instructions++;
    uint8_t op = fetch8();
    switch (op) {
#include "cpu_ops.h"
    }
    if (cyc == 0) cyc = 2;
    if (cpu_stall) {
        cyc += cpu_stall;
        cpu_stall = 0;
    }
    cpu.cycles += (uint32_t)cyc;
    return cyc;
}

int cpu_run(int32_t budget)
{
    int32_t used = 0;
    while (used < budget)
        used += cpu_step();
    return (int)used;
}
