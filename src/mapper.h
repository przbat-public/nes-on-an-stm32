/*
 * mapper.h — cartridge mappers.
 *
 * Mapper 0 (NROM): the ROM is mapped straight through.
 * Mapper 1 (MMC1): five-bit serial writes to $8000-$FFFF select PRG and
 * CHR banks and the nametable mirroring (this is the chip in a large part
 * of the NES library).
 *
 * The mapper never touches memory directly: it publishes bank pointers
 * (nesmem.h) that the inline bus and the PPU read.
 */
#pragma once
#include <stdint.h>

#define MAPPER_NROM 0
#define MAPPER_MMC1 1

/* set up the mapper for a freshly loaded cartridge */
void mapper_init(int number, const uint8_t *prg, uint32_t prg_size,
                 const uint8_t *chr, uint32_t chr_size);

/* writes into $8000-$FFFF (mapper registers) */
void mapper_write(uint16_t addr, uint8_t value);

/* register values, for diagnostics */
extern uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;
