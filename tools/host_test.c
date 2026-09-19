/*
 * host_test.c — runs the 6502 core on the host (no hardware needed).
 *
 * The CPU core only needs bus_read()/bus_write(), so the whole core can
 * be compiled and tested with the system compiler. That is how the CPU
 * was validated before ever touching the board.
 *
 *   make host-test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/cpu6502.h"
#include "cpu_test.h"

/* 64 KB of NES address space */
static uint8_t mem[0x10000];

uint8_t bus_read(uint16_t addr) { return mem[addr]; }
void    bus_write(uint16_t addr, uint8_t v) { mem[addr] = v; }

int main(void)
{
    /* load the self-test program and point the reset vector at it */
    memcpy(mem + CPU_TEST_ORG, cpu_test_program, CPU_TEST_LEN);
    mem[0xFFFC] = CPU_TEST_ORG & 0xFF;
    mem[0xFFFD] = CPU_TEST_ORG >> 8;

    cpu_reset();
    printf("cpu6502 host test — reset vector $%04X, running...\n", cpu.pc);

    /* run 20 000 instructions; the test program then spins in a JMP */
    for (int i = 0; i < 20000; i++)
        cpu_step();

    int fails = 0;
    for (unsigned i = 0; i < CPU_EXPECT_COUNT; i++) {
        const cpu_expect_t *e = &cpu_expects[i];
        uint8_t got = (uint8_t)(mem[e->addr] & e->mask);
        if (got != e->value) {
            printf("FAIL  %-38s $%04X: got $%02X, want $%02X\n",
                   e->name, e->addr, got, e->value);
            fails++;
        } else {
            printf("ok    %-38s $%04X = $%02X\n", e->name, e->addr, got);
        }
    }

    printf("\n%u checks, %d failed — %u instructions, %u cycles "
           "(%u bytes of program)\n",
           (unsigned)CPU_EXPECT_COUNT, fails,
           cpu.instructions, cpu.cycles, (unsigned)CPU_TEST_LEN);
    return fails ? 1 : 0;
}
