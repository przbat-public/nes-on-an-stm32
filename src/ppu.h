/*
 * ppu.h — the NES picture processing unit (2C02).
 *
 * Output convention: ppu_render_scanline() fills ppu_line[256] with NES
 * colour indices (0..63). Turning those into real colours is the display
 * layer's job, which keeps the PPU testable on a PC.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PPU_W 256
#define PPU_H 240

extern uint8_t ppu_line[PPU_W];   /* the scanline just rendered */

void ppu_reset(void);

/* CPU-visible registers $2000..$2007 */
uint8_t ppu_read_reg(uint16_t addr);
void    ppu_write_reg(uint16_t addr, uint8_t v);

/* VRAM window used by the mapper (CHR pattern tables live at $0000) */
uint8_t ppu_read_vram(uint16_t addr);
void    ppu_write_vram(uint16_t addr, uint8_t v);

/* frame stepping */
void ppu_render_scanline(int y);   /* visible lines 0..239             */
void ppu_end_scanline(int y);      /* called after every line (0..261) */
void ppu_set_vblank(void);         /* called when entering vblank      */
void ppu_latch_scroll(void);       /* pre-render: t -> v (scrolling)   */
void ppu_clear_vblank(void);
void ppu_clear_sprite_flags(void); /* pre-render: sprite 0 + overflow  */
bool ppu_nmi_pending(void);
void ppu_clear_nmi(void);

/* reported by the machine layer, so the PPU can pick pattern tables */
extern uint8_t (*ppu_chr_read)(uint16_t addr);
extern void    (*ppu_chr_write)(uint16_t addr, uint8_t v);
extern int      ppu_mirroring;     /* 0 = horizontal, 1 = vertical */

/* rendered output stats (useful for the performance overlay) */
extern uint32_t ppu_frames;
