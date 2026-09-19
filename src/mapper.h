/*
 * mapper.h — cartridge mappers.
 *
 * Mapper 0 (NROM): the ROM is mapped straight through.
 * Mapper 1 (MMC1): five-bit serial writes to $8000-$FFFF select PRG and
 * CHR banks and the nametable mirroring (this is the chip in a large part
 * of the NES library).
 * Mapper 4 (MMC3): bank registers at $8000/$8001, mirroring at $A000, and
 * a scanline counter at $C000-$E001 that raises an IRQ — that counter is
 * how games split the screen (a fixed status bar with a scrolling
 * playfield underneath).
 *
 * The mapper never touches memory directly: it publishes bank pointers
 * (nesmem.h) that the inline bus and the PPU read.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAPPER_NROM 0
#define MAPPER_MMC1 1
#define MAPPER_MMC3 4

/* set up the mapper for a freshly loaded cartridge */
void mapper_init(int number, const uint8_t *prg, uint32_t prg_size,
                 const uint8_t *chr, uint32_t chr_size);

/* writes into $8000-$FFFF (mapper registers) */
void mapper_write(uint16_t addr, uint8_t value);

/* MMC3's IRQ counter is clocked by the PPU once per rendered scanline;
 * the machine layer calls this after every visible line, and hands the
 * interrupt to the CPU when the line is asserted. The flag stays set
 * until the game acknowledges it by writing $E000. */
void mapper_scanline(void);
bool mapper_irq_pending(void);

/* register values, for diagnostics */
extern uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;
extern uint8_t mmc3_select, mmc3_regs_dbg[8], mmc3_irq_latch_dbg;
