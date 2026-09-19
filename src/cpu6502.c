/*
 * cpu6502.c — the emulated 6502 core.
 *
 * Structure:
 *   - the registers live in the file-scope locals below (reg_pc, reg_a,
 *     reg_x, reg_y, reg_sp, reg_p), not in the `cpu` struct: the bus
 *     accesses inside an instruction stop the compiler from keeping
 *     struct fields in host registers, and each field access then costs
 *     a load and a store. cpu_run() copies the locals in once, runs a
 *     whole batch of instructions against them and writes them back
 *     once, which is where the win comes from.
 *   - the addressing helpers below are used by the generated opcode
 *     dispatch in cpu_ops.h (produced by tools/gen_6502.py); they all
 *     work on those locals,
 *   - every instruction ends with CYCLES(n), which records how many
 *     master cycles it burned; cpu_run() uses that for frame pacing.
 *
 * `cpu` still holds the honest current state: every entry point that
 * changes the registers from outside a batch (cpu_reset, cpu_nmi,
 * cpu_irq) syncs through the locals, and cpu_run()/cpu_step() write the
 * locals back when they return. That matters for nes.c, which calls
 * cpu_nmi()/cpu_irq() between cpu_run() batches, and for the SWD
 * debugger, which reads `cpu` while the board runs.
 *
 * The NES's 2A03 has no decimal mode, so the D flag is stored but never
 * changes arithmetic behaviour (as on real hardware).
 */
#include "cpu6502.h"

#ifdef NES_BUS_INLINE
#include "nesmem.h"     /* inlined fast paths for RAM and cartridge ROM */
#endif

cpu_t cpu;

/* ------------------------- register cache ------------------------- */
/* The live copy of the 6502 registers. Their address is never taken,
 * so the compiler may keep them in host registers across the bus
 * accesses of an instruction. */
static uint16_t reg_pc;
static uint8_t  reg_a, reg_x, reg_y, reg_sp, reg_p;

static inline void regs_in(void)
{
    reg_pc = cpu.pc;
    reg_a  = cpu.a;
    reg_x  = cpu.x;
    reg_y  = cpu.y;
    reg_sp = cpu.sp;
    reg_p  = cpu.p;
}

static inline void regs_out(void)
{
    cpu.pc = reg_pc;
    cpu.a  = reg_a;
    cpu.x  = reg_x;
    cpu.y  = reg_y;
    cpu.sp = reg_sp;
    cpu.p  = reg_p;
}

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
static inline uint8_t  fetch8(void)              { return bus_read(reg_pc++); }
static inline uint16_t fetch16(void)
{
    uint16_t lo = fetch8();
    return (uint16_t)(lo | (fetch8() << 8));
}

static inline void setzn(uint8_t v)
{
    reg_p = (uint8_t)((reg_p & ~(F_Z | F_N)) |
                      (v == 0 ? F_Z : 0) | (v & F_N));
}

/* ------------------------------ stack ----------------------------- */

static inline void push(uint8_t v)  { bus_write((uint16_t)(0x0100 + reg_sp--), v); }
static inline uint8_t pull(void)    { return bus_read((uint16_t)(0x0100 + ++reg_sp)); }
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
 * of the operand byte) and updates reg_pc as the instruction requires. */

static inline uint16_t imm(void) { return reg_pc++; }
static inline uint16_t zp(void)  { return fetch8(); }
static inline uint16_t zpx(void) { return (uint16_t)((fetch8() + reg_x) & 0xFF); }
static inline uint16_t zpy(void) { return (uint16_t)((fetch8() + reg_y) & 0xFF); }
static inline uint16_t abs_(void) { return fetch16(); }

static inline uint16_t abx(void)
{
    uint16_t base = fetch16();
    uint16_t a = (uint16_t)(base + reg_x);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

static inline uint16_t aby(void)
{
    uint16_t base = fetch16();
    uint16_t a = (uint16_t)(base + reg_y);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

/* (indirect,X) — pointer read from zero page with wraparound */
static inline uint16_t inx(void)
{
    uint8_t p = (uint8_t)(fetch8() + reg_x);
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
    uint16_t a = (uint16_t)(base + reg_y);
    page = ((base ^ a) & 0xFF00) ? 1 : 0;
    return a;
}

/* branches: returns the cycle count directly */
static int branch(int take)
{
    int8_t off = (int8_t)fetch8();
    if (!take)
        return 2;
    uint16_t old = reg_pc;
    reg_pc = (uint16_t)(reg_pc + off);
    return 3 + (((old ^ reg_pc) & 0xFF00) ? 1 : 0);
}

/* ------------------------------- ALU ------------------------------ */

static void a_ora(uint8_t v) { reg_a |= v;  setzn(reg_a); }
static void a_and(uint8_t v) { reg_a &= v;  setzn(reg_a); }
static void a_eor(uint8_t v) { reg_a ^= v;  setzn(reg_a); }

static void a_adc(uint8_t v)
{
    unsigned carry = (reg_p & F_C) ? 1u : 0u;
    unsigned r = (unsigned)reg_a + v + carry;
    uint8_t  res = (uint8_t)r;

    reg_p &= (uint8_t)~F_C;
    if (r > 0xFF) reg_p |= F_C;
    /* signed overflow: both inputs same sign, result different */
    if (~(reg_a ^ v) & (reg_a ^ res) & 0x80) reg_p |= F_V;
    reg_a = res;
    setzn(res);
}

static void a_sbc(uint8_t v) { a_adc((uint8_t)~v); }

static void cmp_reg(uint8_t r, uint8_t v)
{
    unsigned t = (unsigned)r - v;
    reg_p &= (uint8_t)~F_C;
    if (r >= v) reg_p |= F_C;
    setzn((uint8_t)t);
}

static void a_cmp(uint8_t v) { cmp_reg(reg_a, v); }
static void a_cpx(uint8_t v) { cmp_reg(reg_x, v); }
static void a_cpy(uint8_t v) { cmp_reg(reg_y, v); }

static uint8_t asl(uint8_t v)
{
    reg_p = (uint8_t)((reg_p & ~F_C) | ((v >> 7) & 1));
    uint8_t r = (uint8_t)(v << 1);
    setzn(r);
    return r;
}

static uint8_t lsr(uint8_t v)
{
    reg_p = (uint8_t)((reg_p & ~F_C) | (v & 1));
    uint8_t r = (uint8_t)(v >> 1);
    setzn(r);
    return r;
}

static uint8_t rol(uint8_t v)
{
    uint8_t c = (reg_p & F_C) ? 1 : 0;
    reg_p = (uint8_t)((reg_p & ~F_C) | ((v >> 7) & 1));
    uint8_t r = (uint8_t)((v << 1) | c);
    setzn(r);
    return r;
}

static uint8_t ror(uint8_t v)
{
    uint8_t c = (reg_p & F_C) ? 1 : 0;
    reg_p = (uint8_t)((reg_p & ~F_C) | (v & 1));
    uint8_t r = (uint8_t)((v >> 1) | (c << 7));
    setzn(r);
    return r;
}

static uint8_t inc8(uint8_t v) { v++; setzn(v); return v; }
static uint8_t dec8(uint8_t v) { v--; setzn(v); return v; }

/* --------------------- control-flow instructions ------------------ */

static void brk(void)
{
    push16((uint16_t)(reg_pc + 1));
    push((uint8_t)(reg_p | F_B | F_U));
    reg_p |= F_I;
    reg_pc = (uint16_t)(bus_read(0xFFFE) | (bus_read(0xFFFF) << 8));
}

static void rts(void)
{
    reg_pc = (uint16_t)(pull16() + 1);
}

static void rti(void)
{
    reg_p = (uint8_t)((pull() & ~F_B) | F_U);
    reg_pc = pull16();
}

/* the famous page-boundary bug of the indirect jump */
static void jmp_ind(void)
{
    uint16_t a  = fetch16();
    uint16_t lo = bus_read(a);
    uint16_t hi = bus_read((uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF)));
    reg_pc = (uint16_t)(lo | (hi << 8));
}

/* --------------------------- one instruction ---------------------- */
/* The per-instruction body: fetch the opcode, dispatch through the
 * generated table, charge the cycles. It works on the register cache,
 * and it has exactly one caller — cpu_run()'s batch loop — so inlining
 * it costs no extra code and the six locals stay in host registers for
 * the whole batch. (As an out-of-line call it would be a barrier for the
 * register allocator: the compiler would spill and reload all six at
 * every instruction, and the cache would buy nothing.) */

static inline __attribute__((always_inline)) int exec_one(void)
{
    cyc = 0;
    page = 0;
    uint8_t op = fetch8();
    switch (op) {
#include "cpu_ops.h"
    }
    if (cyc == 0) cyc = 2;
    if (cpu_stall) {
        cyc += cpu_stall;
        cpu_stall = 0;
    }
    return cyc;
}

/* --------------------------- public API --------------------------- */

void cpu_reset(void)
{
    reg_a = reg_x = reg_y = 0;
    reg_sp = 0xFD;
    reg_p  = F_I | F_U;
    reg_pc = (uint16_t)(bus_read(0xFFFC) | (bus_read(0xFFFD) << 8));
    regs_out();
    cpu.cycles = 0;
    cpu.instructions = 0;
    cyc = page = 0;
}

static void do_nmi(void)
{
    push16(reg_pc);
    push((uint8_t)((reg_p & ~F_B) | F_U));
    reg_p |= F_I;
    reg_pc = (uint16_t)(bus_read(0xFFFA) | (bus_read(0xFFFB) << 8));
    cpu.cycles += 7;
}

/* called from the frame loop between cpu_run() batches: the struct is
 * honest there, so sync in, take the interrupt and sync back out */
void cpu_nmi(void)
{
    regs_in();
    do_nmi();
    regs_out();
}

bool cpu_irq(void)
{
    regs_in();
    if (reg_p & F_I) {          /* masked: the line stays asserted */
        regs_out();
        return false;
    }
    push16(reg_pc);
    push((uint8_t)((reg_p & ~F_B) | F_U));
    reg_p |= F_I;
    reg_pc = (uint16_t)(bus_read(0xFFFE) | (bus_read(0xFFFF) << 8));
    cpu.cycles += 7;
    regs_out();
    return true;
}

/* One instruction, with the same external behaviour as always: the
 * registers are synced in, exactly one instruction runs, they are synced
 * back out and the cycle count is returned.
 *
 * A budget of one cycle buys exactly one instruction because every
 * instruction charges at least two (see the `cyc == 0` guard above), so
 * cpu_run() cannot fit a second one into it. */
int cpu_step(void)
{
    return cpu_run(1);
}

/* The hot path: sync the register cache in once, run instructions
 * against it until the cycle budget is spent, sync it back out once.
 * Nothing touches the `cpu` struct inside the loop — that is what lets
 * the compiler keep the six registers in host registers across a whole
 * scanline's worth of instructions. */
int cpu_run(int32_t budget)
{
    int32_t  used = 0;
    uint32_t ins  = 0;

    regs_in();
    while (used < budget) {
        used += exec_one();
        ins++;
    }
    regs_out();

    cpu.instructions += ins;
    cpu.cycles += (uint32_t)used;
    return (int)used;
}
