/*
 * cpu.h — the emulated processor, cut down to what this stage's game uses.
 *
 * Stage 05 explained what a processor is and stage 06 gave it a memory map.
 * This file is that processor again, smaller than the one in the finished
 * emulator and deliberately so: every instruction here is one case in a
 * switch, so you can look up any of them while you read the game's code.
 *
 * The processor knows nothing about the console. It reads and writes bytes
 * through bus_read()/bus_write(), which live in main.c next to the cartridge,
 * and that is the whole reason bank switching works: the processor asks for
 * an address, and somebody else decides which memory that address means
 * today.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The status register is one byte of single-bit flags. Two of them matter
 * for the loops in this stage's game: Z (the last result was zero) and
 * N (the top bit of the last result), because a read of the picture chip's
 * status register reports "a new frame has begun" in bit 7. */
#define FLAG_C 0x01   /* carry: the bit that fell off the top of a sum */
#define FLAG_Z 0x02   /* zero: the last result was zero               */
#define FLAG_I 0x04   /* interrupts off                               */
#define FLAG_D 0x08   /* decimal mode (unused on this chip)           */
#define FLAG_B 0x10   /* break                                        */
#define FLAG_U 0x20   /* unused, always set                           */
#define FLAG_V 0x40   /* signed overflow                              */
#define FLAG_N 0x80   /* negative: the top bit of the last result     */

typedef struct {
    uint8_t  a;         /* the accumulator: where arithmetic happens  */
    uint8_t  x, y;      /* the two index registers                    */
    uint8_t  sp;        /* stack pointer, into page $01               */
    uint8_t  status;
    uint16_t pc;        /* where the next instruction is read from    */
    uint32_t instructions;
} cpu_t;

extern cpu_t cpu;

/* set when cpu_step() gives up: which byte at which address stopped it */
extern uint16_t stopped_at;
extern uint8_t  stopped_opcode;

/* the memory, implemented in main.c */
uint8_t bus_read(uint16_t addr);
void    bus_write(uint16_t addr, uint8_t value);

/* read the reset vector and start there, as the real chip does */
void cpu_reset(void);

/* run one instruction; returns true until the processor hits a stop */
bool cpu_step(void);
