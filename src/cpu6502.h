/*
 * cpu6502.h — the emulated MOS 6502 (as used in the NES).
 *
 * The CPU core is deliberately isolated: it knows nothing about the NES.
 * Every memory access goes through bus_read()/bus_write(), which the
 * machine layer (nes.c) implements.
 */
#pragma once
#include <stdint.h>

/* status register bits */
#define F_C 0x01   /* carry      */
#define F_Z 0x02   /* zero       */
#define F_I 0x04   /* irq disable */
#define F_D 0x08   /* decimal    */
#define F_B 0x10   /* break      */
#define F_U 0x20   /* unused (always 1) */
#define F_V 0x40   /* overflow   */
#define F_N 0x80   /* negative   */

typedef struct {
    uint16_t pc;        /* program counter */
    uint8_t  a, x, y;   /* registers       */
    uint8_t  sp;        /* stack pointer   */
    uint8_t  p;         /* status          */
    uint32_t cycles;    /* total cycles executed (for frame pacing) */
    uint32_t instructions;
} cpu_t;

extern cpu_t cpu;
extern int    cpu_stall;   /* bus-imposed stalls (OAM DMA) */

/* implemented by the machine layer (nes.c) — with NES_BUS_INLINE the
 * fast paths come from nesmem.h instead */
#ifndef NES_BUS_INLINE
uint8_t bus_read(uint16_t addr);
void    bus_write(uint16_t addr, uint8_t value);
#endif

/* interrupts from the PPU/mapper */
void cpu_nmi(void);
void cpu_irq(void);

void cpu_reset(void);
int  cpu_step(void);                 /* one instruction, returns cycles used */
int  cpu_run(int32_t budget);        /* run until >= budget cycles consumed  */
