/*
 * cpu.h — the processor, and the one thing this stage adds to it.
 *
 * Stage 05 built the shape of this processor and stage 13 gave it a cartridge
 * to read its program from. Nothing about the instructions changes here. What
 * changes is that the processor now has a second input: a wire the cartridge
 * can pull to say "stop what you are doing and run this instead". The
 * processor checks that wire between two instructions, never in the middle of
 * one, which is why the program it interrupts can be resumed exactly where it
 * stopped.
 */
#pragma once
#include <stdint.h>

/* One byte of flags. Three of them matter here: Z and N for the branches the
 * game uses, and I, which is the processor's own switch for the interrupt
 * wire. A signal that arrives while I is set waits until it is cleared. */
#define FLAG_C 0x01     /* carry: the bit that fell off the top of a sum */
#define FLAG_Z 0x02     /* zero: the last result was zero               */
#define FLAG_I 0x04     /* interrupts off                               */
#define FLAG_D 0x08     /* decimal mode (this guide never uses it)      */
#define FLAG_B 0x10     /* break                                        */
#define FLAG_U 0x20     /* unused, always set                           */
#define FLAG_V 0x40     /* signed overflow                              */
#define FLAG_N 0x80     /* negative: the top bit of the last result     */

typedef struct {
    uint8_t  a;             /* where arithmetic happens                */
    uint8_t  x, y;          /* the two counters                        */
    uint8_t  sp;            /* stack pointer, into page $01            */
    uint8_t  status;
    uint16_t pc;            /* where the next instruction is read from */
    unsigned long instructions;

    /* An instruction this cut-down processor does not know stops it, and the
     * program in main.c says which one it was. A full processor would have
     * 151 of them; this one has the ones this stage's game uses. */
    int     stopped;
    uint8_t stop_opcode;
} cpu_t;

extern cpu_t cpu;

/* ------------------------------------------------------------------- machine
 * The processor knows nothing about memory, panels or cartridges. It asks
 * these three questions and main.c answers them, which is what stage 06
 * called a bus.
 */
uint8_t machine_read(uint16_t addr);
void    machine_write(uint16_t addr, uint8_t value);
int     machine_irq(void);          /* is the cartridge pulling the wire? */

/* The machine is told when the interrupt is taken, so that a check can see
 * which line of the picture it landed on. */
void    machine_irq_taken(void);

/* Read the reset vector and start there, as the real chip does. */
void cpu_reset(void);

/* Run at most this many instructions. */
void cpu_run(int budget);
