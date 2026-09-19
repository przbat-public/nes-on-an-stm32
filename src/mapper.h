/*
 * mapper.h — cartridge mappers.
 *
 * Mapper 0 (NROM): the ROM is mapped straight through.
 * Mapper 1 (MMC1): five-bit serial writes to $8000-$FFFF select PRG and
 * CHR banks and the nametable mirroring (this is the chip in a large part
 * of the NES library).
 * Mapper 2 (UxROM): the simplest bank switcher there is — writes to
 * $8000-$FFFF select a 16 KB PRG bank at $8000, while $C000 stays on the
 * last bank of the cartridge (which is where the vectors live). CHR is
 * usually 8 KB of RAM on these boards, which the machine layer already
 * handles.
 * Mapper 4 (MMC3): bank registers at $8000/$8001, mirroring at $A000, and
 * a scanline counter at $C000-$E001 that raises an IRQ — that counter is
 * how games split the screen (a fixed status bar with a scrolling
 * playfield underneath).
 * Mapper 5 (MMC5): the kitchen sink. Four PRG modes, four CHR modes with
 * 12 ten-bit bank registers, nametables that live on the cartridge (2 KB
 * of CIRAM plus 1 KB of ExRAM plus a synthesised fill page), 8 KB of
 * banked and write-protected PRG RAM, an 8x8 multiplier, a scanline
 * counter with its own IRQ and a vertical split screen. The nametable
 * hook in ppu.c is the part that reaches outside this file: MMC5 is the
 * only mapper whose nametable data is not the console's CIRAM, so the
 * PPU calls back into here when ppu_mirroring is PPU_MIRROR_MAPPER.
 *
 * The mapper never touches memory directly: it publishes bank pointers
 * (nesmem.h) that the inline bus and the PPU read.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAPPER_NROM 0
#define MAPPER_MMC1 1
#define MAPPER_UXROM 2
#define MAPPER_MMC3 4
#define MAPPER_MMC5 5

/* set up the mapper for a freshly loaded cartridge */
void mapper_init(int number, const uint8_t *prg, uint32_t prg_size,
                 const uint8_t *chr, uint32_t chr_size);

/* writes into $8000-$FFFF (mapper registers) */
void mapper_write(uint16_t addr, uint8_t value);

/* reads from $4020-$5FFF: MMC5 keeps its status, its multiplier result
 * and its 1 KB of ExRAM in the expansion area; every other mapper leaves
 * that window reading as open bus (0) */
uint8_t mapper_read(uint16_t addr);

/* the machine layer forwards writes to the two decoded PPU registers the
 * MMC5 watches ($2000 sprite size, $2001 rendering enable) */
void mapper_ppu_write(uint16_t addr, uint8_t value);

/* writes into $6000-$7FFF (the machine's 8 KB cartridge work RAM window).
 * MMC5 gates them behind $5102/$5103; every other mapper accepts them. */
bool mapper_ram_writable(void);

/* called once from nes_reset(): lets the mapper install its own PPU hooks
 * (MMC5 owns the nametables and reads CHR through its bank registers) */
void mapper_reset(void);

/* MMC5's IRQ counter is clocked by the PPU once per rendered scanline;
 * the machine layer calls this after every visible line, and hands the
 * interrupt to the CPU when the line is asserted. The flag stays set
 * until the game acknowledges it by writing $E000 (MMC3) or reading
 * $5204 (MMC5). */
void mapper_scanline(int y);
bool mapper_irq_pending(void);

/* register values, for diagnostics */
extern uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;
extern uint8_t mmc3_select, mmc3_regs_dbg[8], mmc3_irq_latch_dbg;

/* how far Castlevania III (and any other MMC5 cartridge) actually drives
 * the chip: register writes by group, and how often the PPU had to ask
 * the mapper for nametable data, in an extended-attribute or a split
 * region. Read from the host tool or over SWD. */
extern volatile uint32_t mmc5_dbg_wr_chr_spr, mmc5_dbg_wr_chr_bg;
extern volatile uint32_t mmc5_dbg_wr_prg, mmc5_dbg_wr_chr, mmc5_dbg_wr_nt,
                         mmc5_dbg_wr_exram, mmc5_dbg_wr_irq, mmc5_dbg_wr_mul,
                         mmc5_dbg_wr_split, mmc5_dbg_wr_audio,
                         mmc5_dbg_bg_hook, mmc5_dbg_bg_extattr,
                         mmc5_dbg_bg_split, mmc5_dbg_exram_blank_drop,
                         mmc5_dbg_irq, mmc5_dbg_rd_5204, mmc5_dbg_rd_exram;

/* what the MMC5 last latched, for diagnostics */
extern uint8_t mmc5_regs_dbg[16];
extern uint16_t mmc5_chr_dbg[12];
extern volatile uint32_t mmc5_log[512], mmc5_log_n;
extern volatile uint32_t mmc5_bank_hist[64];
extern volatile uint32_t mmc5_dbg_wr_prot;
void mmc5_dbg_publish(void);
