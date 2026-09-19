/*
 * ppu.h — the NES picture processing unit (2C02).
 *
 * Output convention: ppu_render_scanline() fills the 256-byte buffer it is
 * given with NES colour indices (0..63). Turning those into real colours is
 * the display layer's job, which keeps the PPU testable on a PC. The
 * display layer passes its own framebuffer row so the renderer writes the
 * picture once instead of into a line buffer that then has to be copied;
 * ppu_line is the default row for callers that have no display (the host
 * tools).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PPU_W 256
#define PPU_H 240

extern uint8_t ppu_line[PPU_W];   /* default render target           */

void ppu_reset(void);

/* CPU-visible registers $2000..$2007 */
uint8_t ppu_read_reg(uint16_t addr);
void    ppu_write_reg(uint16_t addr, uint8_t v);

/* VRAM window used by the mapper (CHR pattern tables live at $0000) */
uint8_t ppu_read_vram(uint16_t addr);
void    ppu_write_vram(uint16_t addr, uint8_t v);

/* frame stepping */
void ppu_render_scanline(uint8_t *out, int y);  /* visible lines 0..239 */
void ppu_end_scanline(int y);      /* called after every line (0..261) */
void ppu_set_vblank(void);         /* called when entering vblank      */
void ppu_latch_scroll(void);       /* pre-render: t -> v (scrolling)   */
void ppu_clear_vblank(void);
void ppu_clear_sprite_flags(void); /* pre-render: sprite 0 + overflow  */
bool ppu_rendering_enabled(void);  /* background or sprites are on     */
bool ppu_nmi_pending(void);
void ppu_clear_nmi(void);

/* reported by the machine layer, so the PPU can pick pattern tables */
extern uint8_t (*ppu_chr_read)(uint16_t addr);
extern void    (*ppu_chr_write)(uint16_t addr, uint8_t v);
extern int      ppu_mirroring;     /* 0 = horizontal, 1 = vertical,
                                    * 2/3 = one-screen lower/upper,
                                    * PPU_MIRROR_MAPPER = the cartridge
                                    * owns the nametables (MMC5) */

/* The console's 2 KB of nametable RAM. MMC5 carts put their own CIRAM on
 * the cartridge and address it through $5105, but it is the same memory:
 * the mapper points its CIRAM pages at this. */
uint8_t *ppu_ciram(void);

/* True while the PPU is drawing visible lines with rendering enabled.
 * MMC5 uses it for the two rules that depend on PPU /RD activity: the
 * CPU may only write ExRAM in modes %00/%01 while the picture is being
 * drawn, and the "in frame" status bit follows it. */
bool ppu_visible(void);

/* ------------------- nametable hooks (MMC5 carts) ------------------ *
 * The PPU's own nametable path is a mirroring sum and an array index in
 * the hot loop, and that stays exactly as it is for every mapper whose
 * nametables are the console's CIRAM. MMC5 is the exception: its
 * nametables come from a mapping register, may be 1 KB of on-chip ExRAM
 * or a synthesised fill page, and its extended-attribute mode moves the
 * palette and the CHR bank into a per-tile byte. The mapper installs
 * these hooks and the PPU takes the slower path.
 *
 * ppu_bg_hook() answers one background tile column: it returns the
 * palette, the tile byte (from whichever source $5105 points at) and the
 * pattern address (paddr), and leaves nes_chr[] pointing at whatever CHR
 * bank that tile must be fetched from — extended attributes and the
 * vertical split both change the bank per tile, which is the whole point
 * of those modes. */
#define PPU_MIRROR_MAPPER 4

typedef struct {
    uint16_t nt_addr;    /* PPU address of this tile's nametable fetch */
    uint16_t pat_base;   /* PPU A12 from PPUCTRL ($0000 or $1000)       */
    uint8_t  col;        /* tile column within the line, 0..32          */
    uint8_t  fy;         /* fine Y                                      */
    int      y;          /* visible scanline being rendered             */
} ppu_bg_fetch_t;

extern uint8_t (*ppu_bg_hook)(const ppu_bg_fetch_t *f, uint8_t *tile,
                              uint16_t *paddr);
extern void    (*ppu_chr_bg_hook)(void);    /* rebind CHR for the background */
extern void    (*ppu_chr_spr_hook)(void);   /* rebind CHR for sprites  */
extern uint8_t (*ppu_nt_read_hook)(uint16_t addr);
extern void    (*ppu_nt_write_hook)(uint16_t addr, uint8_t v);

/* rendered output stats (useful for the performance overlay) */
extern uint32_t ppu_frames;
extern volatile uint32_t dbg_bg_tiles, dbg_bg_zero;
extern volatile uint32_t dbg_irq_count, dbg_irq_line, dbg_nmi_count;
extern volatile uint32_t dbg_pad_reads, dbg_pad_writes;
extern volatile uint8_t  dbg_pad_bits[32];
extern volatile uint32_t dbg_pad_bit_n;
extern volatile uint32_t ppu_dbg_ctrl, ppu_dbg_mask;
extern volatile uint32_t dbg_2002_reads, dbg_sprite0_hits, dbg_2007_writes, dbg_2006_writes;
