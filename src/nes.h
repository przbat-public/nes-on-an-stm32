/*
 * nes.h — the machine: CPU + PPU + memory map + cartridge (NROM) + pad.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* pad bits, in shift order (bit 0 first) */
#define PAD_A      0x01
#define PAD_B      0x02
#define PAD_SELECT 0x04
#define PAD_START  0x08
#define PAD_UP     0x10
#define PAD_DOWN   0x20
#define PAD_LEFT   0x40
#define PAD_RIGHT  0x80

/* status codes for nes_load() */
#define NES_OK           0
#define NES_ERR_FORMAT  -1
#define NES_ERR_MAPPER  -2
#define NES_ERR_SIZE    -3

int  nes_load(const uint8_t *rom, uint32_t size);
void nes_reset(void);
void nes_run_frame(void);

/* the display layer installs this: called once per visible scanline with
 * the fresh contents of ppu_line[256] */
extern void (*nes_line_hook)(int y, const uint8_t *line);

/* Where the PPU renders a scanline. The display layer points this at its
 * own framebuffer row, so the picture is written once instead of into a
 * line buffer that then has to be copied; NULL (the host tools) renders
 * into ppu_line as before. */
extern uint8_t *(*nes_line_target)(int y);

void nes_set_buttons(uint8_t mask);

/* $6000-$7FFF traffic, for the MMC5 write-protection question */
extern volatile uint32_t dbg_ram_reads, dbg_ram_writes, dbg_ram_drops;

/* cartridge info (for the on-screen status line) */
extern int nes_mapper;
extern int nes_prg_banks;
extern int nes_chr_banks;

uint8_t *nes_ram(void);          /* 2 KB of CPU RAM (for debug dumps) */

/* The 8 KB cartridge work RAM window at $6000-$7FFF. The mapper needs a
 * pointer to it because MMC5 banks it and can also map it into a PRG ROM
 * window ($5114-$5116 bit 7); every other mapper just lets the machine
 * layer read and write it. */
uint8_t *nes_prg_ram(uint32_t *size);
