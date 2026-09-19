/*
 * diff_test.c — differential test: the old 6502 core against the new one.
 *
 * Both cores are linked into this program (the old one is the committed
 * cpu6502.c, renamed and rebuilt by tools/diff_setup.py). They are fed the
 * same memory contents, the same register/stack/status initial state and
 * exactly the same sequence of operations — a mix of cpu_step(),
 * cpu_run() with 1..4 cycle budgets, cpu_reset(), cpu_nmi(), cpu_irq()
 * and bus-imposed cpu_stall cycles — and after every operation the full
 * architectural state is compared field by field (pc/a/x/y/sp/p plus the
 * cycle and instruction counters). Every 4096 instructions the two 64 KB
 * address spaces are hashed and compared, so a divergence inside a batch
 * cannot hide until the next operation boundary.
 *
 * Each core has its own console RAM (that is the point: the old core's
 * fast path reads its own array). The PRG windows are shared read-only
 * bytes, and the $2000+ slow path returns a function of the address
 * alone, so both cores see identical reads there.
 *
 * The canned stalls are keyed off the *instruction count* of the core
 * being run, not off a bus-access counter: both cores count instructions
 * identically, so they get the same stall at the same instruction even
 * if they were to diverge in memory — which keeps a memory divergence a
 * detectable state difference instead of turning it into a stall
 * difference that would mask it.
 *
 * Build (tools/diff_run.sh does all of this):
 *   cc -O2 -Wall -Wextra -Isrc -Ibuild/difftest/old_cpu_core \
 *      -DNES_BUS_INLINE -o build/difftest/diff_test \
 *      tools/diff_test.c build/difftest/old_cpu_core/cpu6502.c \
 *      src/cpu6502.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/nesmem.h"          /* the new core's view of memory */
#include "../src/cpu6502.h"         /* cpu_t, the new core's API      */

/* The old core's cpu6502.h is deliberately NOT included: the rename
 * macros in diff_prefix.h (included below, after the harness's own bus
 * functions are defined) would rewrite this file's declarations too.
 * The old struct is identical to cpu_t, so the harness declares it and
 * the old entry points under their renamed names, and the static
 * assertions below pin the layout down. */
typedef struct {                    /* == the old core's cpu_t */
    uint16_t pc;
    uint8_t  a, x, y;
    uint8_t  sp;
    uint8_t  p;
    uint32_t cycles;
    uint32_t instructions;
} old_cpu_t;

extern old_cpu_t OLD_cpu;
extern int       OLD_cpu_stall;
void     OLD_cpu_reset(void);
int      OLD_cpu_step(void);
int      OLD_cpu_run(int32_t budget);
void     OLD_cpu_nmi(void);
bool     OLD_cpu_irq(void);

/* the two structs must be laid out identically for the pointer cast
 * between them (and for memcmp of the snapshots) to be sound */
_Static_assert(sizeof(old_cpu_t) == sizeof(cpu_t),
               "old/new cpu_t size mismatch");
_Static_assert(sizeof(old_cpu_t) == 16, "old/new cpu_t layout mismatch");

/* -------------------------- the new core's bus -------------------- */
uint8_t  nes_ram_2k[0x800];
const uint8_t *nes_prg[4];
uint8_t       *nes_chr_ram;
int            nes_chr_is_ram;
const uint8_t *nes_chr[8];

/* -------------------------- the old core's bus -------------------- */
uint8_t  OLD_nes_ram_2k[0x800];
const uint8_t *OLD_nes_prg[4];

static uint8_t prg_image[0x8000];
static unsigned long slow_reads, slow_writes;

uint8_t nes_bus_read_slow(uint16_t addr)
{
    if (addr < 0x2000)
        return nes_ram_2k[addr & 0x7FF];
    slow_reads++;
    return (uint8_t)((addr * 31u + (addr >> 3)) & 0xFF);
}

void nes_bus_write_slow(uint16_t addr, uint8_t v)
{
    (void)addr; (void)v;
    slow_writes++;
}

uint8_t OLD_nes_bus_read_slow(uint16_t addr)
{
    if (addr < 0x2000)
        return OLD_nes_ram_2k[addr & 0x7FF];
    slow_reads++;
    return (uint8_t)((addr * 31u + (addr >> 3)) & 0xFF);
}

void OLD_nes_bus_write_slow(uint16_t addr, uint8_t v)
{
    (void)addr; (void)v;
    slow_writes++;
}

/* --------------------------- op mixing ---------------------------- */
static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(void)
{
    /* xorshift32: same sequence on every run and every machine */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* canned bus stalls: a nonzero stall is charged for a run of consecutive
 * instructions starting a little way into the program (so the stall path is
 * exercised, and by a pile-up rather than a single spike) */
static int  pending_stall;
static unsigned stall_runs;

static int stall_for(uint32_t ins)
{
    if (pending_stall > 0) {
        pending_stall--;
        return 1 + (int)(stall_runs % 5u);
    }
    if (ins >= 32 && (ins % 11u) == 0) {
        stall_runs++;
        pending_stall = 1 + (int)((ins / 11u) % 3u);
        return 1 + (int)(stall_runs % 5u);
    }
    return 0;
}

static void hash_bytes(uint32_t *h, const uint8_t *p, size_t n)
{
    uint32_t v = *h;
    for (size_t i = 0; i < n; i++) {
        v ^= p[i];
        v *= 16777619u;
    }
    *h = v;
}

static void hash_both(uint32_t *hn, uint32_t *ho)
{
    uint32_t a = 2166136261u, b = 2166136261u;
    hash_bytes(&a, nes_ram_2k, sizeof nes_ram_2k);
    hash_bytes(&a, prg_image, sizeof prg_image);
    hash_bytes(&b, OLD_nes_ram_2k, sizeof OLD_nes_ram_2k);
    hash_bytes(&b, prg_image, sizeof prg_image);
    *hn = a;
    *ho = b;
}

/* fill the PRG image with plausible bytes: two opcode-ish bytes in every
 * four so real instructions (not only illegal NOPs) execute, and vectors
 * pointing into the ROM windows */
static void build_prg(uint8_t *prg, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((i & 3) < 2) {
            static const uint8_t ops[] = {
                0xA9, 0x85, 0xAD, 0xBD, 0xB1, 0x69, 0xE9, 0x29, 0x09,
                0x49, 0x0A, 0x2A, 0x4A, 0x6A, 0xE6, 0xC6, 0xEE, 0xCE,
                0x18, 0x38, 0xD0, 0xF0, 0x10, 0x30, 0x20, 0x60, 0x68,
                0x48, 0x08, 0x28, 0xAA, 0x8A, 0xE8, 0xC8, 0x88, 0xCA,
                0x9D, 0x99, 0x91, 0x81, 0x0D, 0x1D, 0x19, 0x11, 0x01,
                0x2D, 0x3D, 0x39, 0x31, 0x21, 0x6D, 0x7D, 0x79, 0x71,
                0x61, 0xCD, 0xDD, 0xD9, 0xD1, 0xC1, 0xE0, 0xC0, 0x24,
                0x2C, 0x4C, 0x6C, 0x40, 0x00, 0x03, 0x07, 0x0F, 0x1F,
                0x1B, 0x13, 0x23, 0x27, 0x2F, 0x3F, 0x3B, 0x33, 0x43,
                0x47, 0x4F, 0x5F, 0x5B, 0x53, 0x63, 0x67, 0x6F, 0x7F,
                0x7B, 0x73, 0x83, 0x87, 0x8F, 0x97, 0xA7, 0xAF, 0xB7,
                0xBF, 0xA3, 0xB3, 0xC3, 0xC7, 0xCF, 0xDF, 0xDB, 0xD3,
                0xE3, 0xE7, 0xEF, 0xFF, 0xFB, 0xF3, 0x96, 0x94, 0x8C,
                0xB4, 0xBC, 0xA4, 0xB6, 0xBE, 0xA6, 0xAE, 0xAC, 0x86,
                0x8E, 0x84, 0x95, 0x86, 0xB8, 0xD8, 0xF8, 0x58, 0x78,
                0xEA, 0x9A, 0xBA, 0x98, 0x8D, 0x8B, 0xE4, 0xC4, 0xCC,
                0xEC, 0x75, 0x65, 0xE5, 0xC5, 0xF5, 0xD5, 0x35, 0x25,
            };
            prg[i] = ops[(i * 7 + (i >> 4)) % (sizeof ops)];
        } else {
            prg[i] = (uint8_t)(i * 37u + (i >> 5));
        }
    }
    /* vectors: reset at 0xC000 (window 2), NMI/IRQ at 0xE000 (window 3) */
    prg[0x7FFA] = 0x00; prg[0x7FFB] = 0xE0;
    prg[0x7FFC] = 0x00; prg[0x7FFD] = 0xC0;
    prg[0x7FFE] = 0x00; prg[0x7FFF] = 0xE0;
}

/* ------------------------------- main ----------------------------- */
typedef struct {
    uint16_t pc;
    uint8_t  a, x, y, sp, p;
    uint32_t cycles, instructions;
} snap_t;

static snap_t snap_new(void)
{
    snap_t s;
    s.pc = cpu.pc; s.a = cpu.a; s.x = cpu.x; s.y = cpu.y;
    s.sp = cpu.sp; s.p = cpu.p;
    s.cycles = cpu.cycles;
    s.instructions = cpu.instructions;
    return s;
}

static snap_t snap_old(void)
{
    snap_t s;
    s.pc = OLD_cpu.pc; s.a = OLD_cpu.a; s.x = OLD_cpu.x; s.y = OLD_cpu.y;
    s.sp = OLD_cpu.sp; s.p = OLD_cpu.p;
    s.cycles = OLD_cpu.cycles;
    s.instructions = OLD_cpu.instructions;
    return s;
}

static void report_diff(const char *what, unsigned long step,
                        const snap_t *o, const snap_t *n)
{
    printf("MISMATCH after %s at step %lu\n", what, step);
    printf("  old: pc=%04X a=%02X x=%02X y=%02X sp=%02X p=%02X "
           "cyc=%u ins=%u\n", o->pc, o->a, o->x, o->y, o->sp, o->p,
           o->cycles, o->instructions);
    printf("  new: pc=%04X a=%02X x=%02X y=%02X sp=%02X p=%02X "
           "cyc=%u ins=%u\n", n->pc, n->a, n->x, n->y, n->sp, n->p,
           n->cycles, n->instructions);
}

/* ---------------------- exhaustive opcode sweep ------------------- */
/* Runs every one of the 256 opcodes from RAM ($0400, so the operand
 * bytes are under the harness's control) once per combination of
 * registers, flags, stack pointer and operand bytes, and compares the
 * full state after each single instruction. The random mix above reaches
 * the common paths often and the rare ones occasionally; this reaches
 * every opcode under many flag/operand combinations. */
static int sweep(unsigned trials)
{
    unsigned long checked = 0, bad = 0;

    for (int op = 0; op < 256 && bad == 0; op++) {
        for (unsigned t = 0; t < trials && bad == 0; t++) {
            /* rebuild the RAM image so every trial starts identical */
            for (unsigned i = 0; i < sizeof nes_ram_2k; i++)
                nes_ram_2k[i] = (uint8_t)(i * 89u + 7u + t * 31u + op);
            /* vectors (top of RAM region is mirrored there as well) */
            nes_ram_2k[0x7FA] = 0x00; nes_ram_2k[0x7FB] = 0x04;
            nes_ram_2k[0x7FC] = 0x00; nes_ram_2k[0x7FD] = 0x04;
            nes_ram_2k[0x7FE] = 0x00; nes_ram_2k[0x7FF] = 0x04;
            /* the program: opcode at $0400, operand bytes after it */
            nes_ram_2k[0x400] = (uint8_t)op;
            for (int i = 1; i <= 8; i++)
                nes_ram_2k[0x400 + i] = (uint8_t)(t * 7u + i * 3u);
            /* the zero-page operand pair, so ZP/INX/INY have a pointer */
            nes_ram_2k[0x10] = 0x00;
            nes_ram_2k[0x11] = 0x04;
            nes_ram_2k[0x12] = 0xFF;
            nes_ram_2k[0x13] = 0x03;
            nes_ram_2k[0x14] = 0x40;
            nes_ram_2k[0x15] = 0x02;
            memcpy(OLD_nes_ram_2k, nes_ram_2k, sizeof nes_ram_2k);

            uint8_t a = (uint8_t)(t * 37u + 0x11);
            uint8_t x = (uint8_t)(t * 53u + 0x80);
            uint8_t y = (uint8_t)(t * 29u + 0xC3);
            uint8_t sp = (uint8_t)(0xFD - (t % 5u));
            uint8_t p  = (uint8_t)(t * 13u + 0x20);
            memset(&cpu, 0, sizeof cpu);
            cpu.pc = 0x0400; cpu.a = a; cpu.x = x; cpu.y = y;
            cpu.sp = sp; cpu.p = p;
            memcpy(&OLD_cpu, &cpu, sizeof cpu);
            cpu_stall = OLD_cpu_stall = 0;

            int c1 = cpu_step();
            int c2 = OLD_cpu_step();
            checked++;
            if (c1 != c2) {
                printf("SWEEP: opcode $%02X trial %u: cycles %d (new) vs %d (old)\n",
                       op, t, c1, c2);
                bad++;
                break;
            }
            snap_t n = snap_new(), o = snap_old();
            if (memcmp(&n, &o, sizeof n) != 0) {
                printf("SWEEP: opcode $%02X trial %u (a=%02X x=%02X y=%02X "
                       "sp=%02X p=%02X, operands %02X %02X %02X %02X)\n",
                       op, t, a, x, y, sp, p, nes_ram_2k[0x401],
                       nes_ram_2k[0x402], nes_ram_2k[0x403],
                       nes_ram_2k[0x404]);
                report_diff("one swept instruction", checked, &o, &n);
                bad++;
                break;
            }
            if (memcmp(nes_ram_2k, OLD_nes_ram_2k, sizeof nes_ram_2k) != 0) {
                printf("SWEEP: opcode $%02X trial %u: RAM differs\n", op, t);
                for (unsigned i = 0; i < sizeof nes_ram_2k; i++) {
                    if (nes_ram_2k[i] != OLD_nes_ram_2k[i]) {
                        printf("  RAM[$%04X]: new %02X, old %02X\n", i,
                               nes_ram_2k[i], OLD_nes_ram_2k[i]);
                        break;
                    }
                }
                bad++;
                break;
            }
        }
    }
    if (bad)
        return 1;
    printf("SWEEP PASSED: all 256 opcodes x %u combinations = %lu single "
           "instructions, identical state and RAM\n", trials, checked);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long target_ins = (argc > 1) ? strtoul(argv[1], NULL, 0) : 5000000ul;
    int verbose = 0, do_sweep = 0;
    unsigned sweep_trials = 24;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "sweep") == 0)
            do_sweep = 1;
        else if (strncmp(argv[i], "sweep=", 6) == 0) {
            sweep_trials = (unsigned)strtoul(argv[i] + 6, NULL, 0);
            do_sweep = 1;
        }
        else if (strncmp(argv[i], "seed=", 5) == 0)
            rng_state = (uint32_t)strtoul(argv[i] + 5, NULL, 0) | 1u;
    }

    /* ---- one memory image, handed to both cores ---- */
    build_prg(prg_image, sizeof prg_image);
    for (int i = 0; i < 4; i++) {
        nes_prg[i] = prg_image + i * 0x2000;
        OLD_nes_prg[i] = prg_image + i * 0x2000;
    }
    for (unsigned i = 0; i < sizeof nes_ram_2k; i++)
        nes_ram_2k[i] = (uint8_t)(i * 89u + 7u);
    memcpy(OLD_nes_ram_2k, nes_ram_2k, sizeof nes_ram_2k);
    /* stack area: the two cores pull/push through their own RAM arrays */
    nes_ram_2k[0x100] = 0x00;
    OLD_nes_ram_2k[0x100] = 0x00;

    /* ---- same initial registers in both cores ---- */
    cpu.pc = 0xC000; cpu.a = 0x00; cpu.x = 0x00; cpu.y = 0x00;
    cpu.sp = 0xFD;   cpu.p = 0x24; cpu.cycles = 0; cpu.instructions = 0;
    memcpy(&OLD_cpu, &cpu, sizeof cpu);   /* same layout, checked above */
    OLD_cpu_stall = 0;

    printf("differential test: old cpu6502.c vs new cpu6502.c\n");
    if (do_sweep) {
        int rc = sweep(sweep_trials);
        if (rc != 0) {
            printf("\nSWEEP FAILED\n");
            return rc;
        }
    }
    printf("  target %lu instructions, reset vector $%04X, NMI/IRQ $E000\n",
           target_ins, cpu.pc);

    unsigned long steps = 0, mismatches = 0, executed = 0;
    uint32_t hash_new = 0, hash_old = 0;
    unsigned long last_hash_ins = 0, hash_checks = 0;
    unsigned long op_counts[6] = {0, 0, 0, 0, 0, 0};

    while (executed < target_ins && mismatches == 0) {
        steps++;
        unsigned op = rnd() % 1001;
        unsigned which;

        /* the same canned stall for both cores, keyed off the instruction
         * count they are about to run from */
        cpu_stall = OLD_cpu_stall = stall_for(cpu.instructions);

        if (op < 550) {                    /* one instruction */
            which = 0;
            int c1 = cpu_step();
            int c2 = OLD_cpu_step();
            executed += 1;                 /* cpu_step runs one */
            if (c1 != c2) {
                printf("cpu_step returned %d (new) vs %d (old) at step %lu\n",
                       c1, c2, steps);
                mismatches++;
                break;
            }
        } else if (op < 990) {             /* a batch of 1..4 cycles */
            which = 1;
            int32_t budget = (int32_t)(1 + rnd() % 4);
            uint32_t before = cpu.instructions;
            int c1 = cpu_run(budget);
            int c2 = OLD_cpu_run(budget);
            executed += cpu.instructions - before;
            if (c1 != c2) {
                printf("cpu_run(%d) returned %d (new) vs %d (old) at step %lu\n",
                       (int)budget, c1, c2, steps);
                mismatches++;
                break;
            }
        } else if (op < 992) {             /* reset: reads the vector */
            which = 2;
            cpu_reset();
            OLD_cpu_reset();
        } else if (op < 995) {             /* NMI */
            which = 3;
            cpu_nmi();
            OLD_cpu_nmi();
        } else if (op < 998) {             /* IRQ (may be masked) */
            which = 4;
            bool t1 = cpu_irq();
            bool t2 = OLD_cpu_irq();
            if (t1 != t2) {
                printf("cpu_irq returned %d (new) vs %d (old) at step %lu\n",
                       (int)t1, (int)t2, steps);
                mismatches++;
                break;
            }
        } else if (op == 998) {            /* budget edge cases */
            which = 1;
            static const int32_t budgets[] = {0, -1, -1000, 100000};
            int32_t budget = budgets[rnd() % 4];
            uint32_t before = cpu.instructions;
            int c1 = cpu_run(budget);
            int c2 = OLD_cpu_run(budget);
            executed += cpu.instructions - before;
            if (c1 != c2) {
                printf("cpu_run(%d) returned %d (new) vs %d (old) at step %lu\n",
                       (int)budget, c1, c2, steps);
                mismatches++;
                break;
            }
        } else {                           /* reset, then a batch */
            which = 5;
            cpu_reset();
            OLD_cpu_reset();
            uint32_t before = cpu.instructions;
            cpu_run(3);
            OLD_cpu_run(3);
            executed += cpu.instructions - before;
        }
        op_counts[which]++;

        snap_t n = snap_new(), o = snap_old();
        if (memcmp(&n, &o, sizeof n) != 0) {
            report_diff("operation", steps, &o, &n);
            mismatches++;
            break;
        }

        if (executed - last_hash_ins >= 4096) {
            hash_both(&hash_new, &hash_old);
            hash_checks++;
            if (hash_new != hash_old) {
                printf("MEMORY HASH MISMATCH after %lu instructions "
                       "(new %08X, old %08X)\n",
                       executed, hash_new, hash_old);
                mismatches++;
                break;
            }
            last_hash_ins = executed;
            if (verbose && (hash_checks % 100) == 0) {
                printf("  ... %lu instructions, hashes %08X, ins new %u old %u\n",
                       executed, hash_new, cpu.instructions,
                       OLD_cpu.instructions);
                fflush(stdout);
            }
        }
    }

    printf("\n");
    printf("  operations: %lu (step %lu, run %lu, reset %lu, nmi %lu, "
           "irq %lu, reset+run %lu)\n",
           steps, op_counts[0], op_counts[1], op_counts[2], op_counts[3],
           op_counts[4], op_counts[5]);
    printf("  instructions: new %u, old %u\n",
           cpu.instructions, OLD_cpu.instructions);
    printf("  cycles:       new %u, old %u\n", cpu.cycles, OLD_cpu.cycles);
    printf("  final state:  new pc=%04X a=%02X x=%02X y=%02X sp=%02X p=%02X\n",
           cpu.pc, cpu.a, cpu.x, cpu.y, cpu.sp, cpu.p);
    printf("                old pc=%04X a=%02X x=%02X y=%02X sp=%02X p=%02X\n",
           OLD_cpu.pc, OLD_cpu.a, OLD_cpu.x, OLD_cpu.y, OLD_cpu.sp, OLD_cpu.p);
    printf("  memory hashes checked every 4096 instructions: %lu\n", hash_checks);
    printf("  memory hash:  new %08X, old %08X\n", hash_new, hash_old);
    printf("  bus slow-path accesses: %lu reads, %lu writes\n",
           slow_reads, slow_writes);
    if (mismatches) {
        printf("\nDIFFERENTIAL TEST FAILED (%lu mismatch%s)\n",
               mismatches, mismatches == 1 ? "" : "es");
        return 1;
    }
    printf("\nDIFFERENTIAL TEST PASSED: %lu instructions executed, %lu "
           "operations, identical registers, cycle counts and memory hashes\n",
           executed, steps);
    return 0;
}
