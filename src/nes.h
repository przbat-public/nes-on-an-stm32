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

void nes_set_buttons(uint8_t mask);

/* cartridge info (for the on-screen status line) */
extern int nes_mapper;
extern int nes_prg_banks;
extern int nes_chr_banks;

uint8_t *nes_ram(void);          /* 2 KB of CPU RAM (for debug dumps) */
