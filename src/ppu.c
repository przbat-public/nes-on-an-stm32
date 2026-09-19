/*
 * ppu.c — the NES PPU, implemented as a scanline renderer.
 *
 * Why scanline based: a real PPU draws 256 pixels per line with its own
 * internal timing, and games change scroll/palette registers *during* the
 * frame (status bars, screen splits). Rendering whole frames at once
 * would make those tricks impossible, so we render one line at a time,
 * exactly between the CPU's per-line time slices.
 *
 * Implemented: nametables + mirroring, attribute tables, background and
 * sprite rendering (8x8 and 8x16), flips, priority, sprite overflow,
 * sprite-0 hit, the loopy scroll registers (v/t/x/w), $2007 read buffer
 * and palette mirroring.
 */
#include "ppu.h"
#include "nesmem.h"      /* inline chr_read() for the pattern fetches */

uint8_t  ppu_line[PPU_W];
uint32_t ppu_frames;

uint8_t (*ppu_chr_read)(uint16_t addr);
void    (*ppu_chr_write)(uint16_t addr, uint8_t v);
int      ppu_mirroring;

/* ------------------------------- state ---------------------------- */

static uint8_t  vram[0x800];      /* 2 KB nametables (mirrored)     */
static uint8_t  pal[32];          /* palette RAM                    */
static uint8_t  pal_cache[32];    /* same values, masked: the renderer
                                   * reads these 61k times per frame
                                   * instead of calling a function      */
static uint8_t  oam[256];         /* sprite memory                  */
static uint8_t  oam_addr;

static uint8_t  ctrl;             /* $2000 */
static uint8_t  mask;             /* $2001 */
static uint8_t  status;           /* $2002 */
static uint8_t  read_buffer;      /* $2007 buffered read            */

/* loopy scroll registers */
static uint16_t v, t;             /* current / temporary VRAM address */
static uint8_t  fine_x;
static uint8_t  w;                /* first/second write toggle      */

static bool     nmi_pending;
static bool     nmi_occurred;     /* for the $2002 read behaviour   */
static uint8_t  suppress_vblank;

/* ---------------------------- nametables -------------------------- */

/* 0 = horizontal, 1 = vertical, 2 = one-screen lower, 3 = one-screen
 * upper (MMC1 carts switch between these while the game runs) */
static uint16_t nt_index(uint16_t addr)
{
    uint16_t a = (uint16_t)((addr - 0x2000) & 0x0FFF);
    switch (ppu_mirroring) {
    case 1:  return (uint16_t)(a & 0x7FF);                       /* vertical */
    case 2:  return (uint16_t)(a & 0x3FF);                       /* NT0      */
    case 3:  return (uint16_t)(0x400 | (a & 0x3FF));             /* NT1      */
    default: return (uint16_t)((((a >> 11) & 1) << 10) | (a & 0x3FF));
    }
}

uint8_t ppu_read_vram(uint16_t addr)
{
    addr &= 0x3FFF;
    if (addr < 0x2000) return ppu_chr_read(addr);
    if (addr < 0x3F00) return vram[nt_index(addr)];
    /* palette: $3F10/$14/$18/$1C mirror $3F00/$04/$08/$0C */
    uint8_t p = (uint8_t)(addr & 0x1F);
    if ((p & 0x13) == 0x10) p &= 0x0F;
    return (uint8_t)(pal[p] & 0x3F);
}

void ppu_write_vram(uint16_t addr, uint8_t val)
{
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        ppu_chr_write(addr, val);
    } else if (addr < 0x3F00) {
        vram[nt_index(addr)] = val;
    } else {
        uint8_t p = (uint8_t)(addr & 0x1F);
        if ((p & 0x13) == 0x10) p &= 0x0F;
        pal[p] = (uint8_t)(val & 0x3F);
        pal_cache[p] = (uint8_t)(val & 0x3F);
    }
}

/* ------------------------------ registers ------------------------- */

static void increment_v(void)
{
    v = (uint16_t)(v + ((ctrl & 0x04) ? 32 : 1));
}

uint8_t ppu_read_reg(uint16_t addr)
{
    uint8_t r = 0;
    switch (addr & 7) {
    case 2:                                   /* PPUSTATUS */
        r = (uint8_t)((status & 0xE0) | (read_buffer & 0x1F));
        status &= (uint8_t)~0x80;             /* vblank read clears it */
        nmi_occurred = false;
        w = 0;
        break;
    case 4:                                   /* OAMDATA */
        r = oam[oam_addr];
        break;
    case 7:                                   /* PPUDATA */
        r = read_buffer;
        read_buffer = ppu_read_vram(v);
        if ((v & 0x3FFF) >= 0x3F00)           /* palette reads are direct */
            r = read_buffer;
        increment_v();
        break;
    default:
        break;
    }
    return r;
}

void ppu_write_reg(uint16_t addr, uint8_t val)
{
    switch (addr & 7) {
    case 0:                                   /* PPUCTRL */
        ctrl = val;
        t = (uint16_t)((t & 0xF3FF) | ((val & 3) << 10));
        if ((val & 0x80) && (status & 0x80) && !nmi_occurred) {
            nmi_pending = true;
            nmi_occurred = true;
        }
        break;
    case 1:                                   /* PPUMASK */
        mask = val;
        break;
    case 3:                                   /* OAMADDR */
        oam_addr = val;
        break;
    case 4:                                   /* OAMDATA */
        oam[oam_addr++] = val;
        break;
    case 5:                                   /* PPUSCROLL */
        if (!w) {
            t = (uint16_t)((t & 0xFFE0) | (val >> 3));
            fine_x = (uint8_t)(val & 7);
        } else {
            t = (uint16_t)((t & 0x8FFF) | ((val & 7) << 12));
            t = (uint16_t)((t & 0xFC1F) | ((val & 0xF8) << 2));
        }
        w ^= 1;
        break;
    case 6:                                   /* PPUADDR */
        if (!w) {
            t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8));
        } else {
            t = (uint16_t)((t & 0xFF00) | val);
            v = t;
        }
        w ^= 1;
        break;
    case 7:                                   /* PPUDATA */
        ppu_write_vram(v, val);
        increment_v();
        break;
    default:
        break;
    }
}

/* ------------------------------- sprites -------------------------- */

#define SPRITE_H() ((ctrl & 0x20) ? 16 : 8)

typedef struct {
    uint8_t pixels[8];    /* two bitplanes, already flipped */
    uint8_t palette;      /* 0..3 */
    bool    behind;
} sprite_row_t;

/* Fetch one visible sprite's scanline once, then blend it pixel by
 * pixel. Doing the pattern fetch per sprite row instead of per pixel is
 * what made sprites affordable on an 80 MHz CPU. */
static void sprite_fetch(int i, int row, sprite_row_t *out)
{
    uint8_t tile = oam[i * 4 + 1];
    uint8_t attr = oam[i * 4 + 2];
    int r = row, c = 0;

    if (attr & 0x80) r = (SPRITE_H() - 1) - r;      /* flip vertical   */
    c = (attr & 0x40) ? -1 : 1;                     /* flip horizontal */

    uint16_t base = (ctrl & 0x08) ? 0x1000 : 0x0000;
    uint16_t addr;
    if (SPRITE_H() == 16) {
        base = (uint16_t)((tile & 1) ? 0x1000 : 0x0000);
        addr = (uint16_t)(base + ((tile & 0xFE) * 16) + (r >= 8 ? 16 : 0)
                          + (r & 7));
    } else {
        addr = (uint16_t)(base + tile * 16 + (r & 7));
    }

    uint8_t lo = chr_read(addr);
    uint8_t hi = chr_read((uint16_t)(addr + 8));
    int bit = (c == 1) ? 7 : 0;
    for (int k = 0; k < 8; k++) {
        out->pixels[k] = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
        bit -= c;
    }
    out->palette = (uint8_t)(attr & 3);
    out->behind  = (attr & 0x20) != 0;
}

/* --------------------------- scanline render ---------------------- */

void ppu_render_scanline(int y)
{
    uint8_t backdrop = pal_cache[0];

    for (int i = 0; i < PPU_W; i++)
        ppu_line[i] = backdrop;

    bool bg_on = (mask & 0x08) != 0;
    bool sp_on = (mask & 0x10) != 0;

    /* ---------------- background ---------------- */
    if (bg_on) {
        /* Work out the nametable position once, then walk it: tile bytes
         * inside a row are contiguous in VRAM, and a coarse-X wrap just
         * flips the 1 KB nametable bit (bit 10 of the index). */
        int cx  = v & 0x1F;
        int cy  = (v >> 5) & 0x1F;
        int fy  = (v >> 12) & 7;
        uint16_t pat_base = (ctrl & 0x10) ? 0x1000 : 0x0000;
        uint16_t nt_idx = nt_index((uint16_t)(0x2000 | (v & 0x0FFF)));
        int at_row = 0x3C0 | ((cy >> 2) << 3);

        for (int col = 0; col < 33; col++) {
            uint8_t tile = vram[nt_idx];
            /* the attribute table follows the nametable select bit, so it
             * must be recomputed after a coarse-X wrap */
            uint8_t attr = vram[at_row | (nt_idx & 0x400) | (cx >> 2)];
            uint8_t palette = (uint8_t)((attr >> (((cy & 2) << 1) | (cx & 2))) & 3);

            const uint8_t *pc = &pal_cache[palette * 4];
            uint8_t c1 = pc[1], c2 = pc[2], c3 = pc[3];

            uint16_t paddr = (uint16_t)(pat_base + tile * 16 + fy);
            uint8_t lo = chr_read(paddr);
            uint8_t hi = chr_read((uint16_t)(paddr + 8));
            int start = col * 8 - fine_x;

            /* the common case: the whole tile is on screen */
            if (start >= 0 && start <= PPU_W - 8) {
                uint8_t *out = &ppu_line[start];
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t b = (uint8_t)(7 - bit);
                    uint8_t pix = (uint8_t)(((lo >> b) & 1) |
                                            (((hi >> b) & 1) << 1));
                    if (pix)
                        out[bit] = (pix == 1) ? c1 : (pix == 2) ? c2 : c3;
                }
            } else {
                for (int bit = 0; bit < 8; bit++) {
                    int px = start + bit;
                    if (px < 0 || px >= PPU_W)
                        continue;
                    uint8_t b = (uint8_t)(7 - bit);
                    uint8_t pix = (uint8_t)(((lo >> b) & 1) |
                                            (((hi >> b) & 1) << 1));
                    if (pix)
                        ppu_line[px] = (pix == 1) ? c1 : (pix == 2) ? c2 : c3;
                }
            }

            /* advance to the next tile column: inside a row the VRAM
             * index just increments; after the last column it wraps to
             * the other nametable's same row */
            if (cx == 31)
                nt_idx = (uint16_t)((nt_idx - 31) ^ 0x400);
            else
                nt_idx++;
            cx = (cx + 1) & 31;
        }
    }

    /* ---------------- sprites ---------------- */
    if (sp_on) {
        int h = SPRITE_H();
        int count = 0;
        for (int i = 0; i < 64; i++) {
            int sy = oam[i * 4 + 0] + 1;
            if (y < sy || y >= sy + h)
                continue;
            count++;
            if (count > 8) {
                /* the real PPU raises the overflow flag (buggy, but the
                 * flag is what games poll) */
                status |= 0x20;
                break;
            }
            int sx = oam[i * 4 + 3];
            if (sx >= PPU_W)
                continue;

            sprite_row_t spr;
            sprite_fetch(i, y - sy, &spr);
            const uint8_t *pc = &pal_cache[16 + spr.palette * 4];

            int n = PPU_W - sx;
            if (n > 8) n = 8;
            for (int c = 0; c < n; c++) {
                uint8_t pix = spr.pixels[c];
                if (pix == 0)
                    continue;
                int px = sx + c;
                bool bg_opaque = bg_on && ppu_line[px] != backdrop;

                if (i == 0 && bg_opaque && px < 255)
                    status |= 0x40;                 /* sprite 0 hit */

                if (spr.behind && bg_opaque)
                    continue;
                ppu_line[px] = pc[pix];
            }
        }
    }
}

/* ------------------------- per-line scrolling --------------------- */

volatile uint32_t dbg_mask, dbg_ctrl, dbg_v, dbg_t;

void ppu_end_scanline(int y)
{
    if (y == 261) { dbg_mask = mask; dbg_ctrl = ctrl; dbg_v = v; dbg_t = t; }
    /* v advances only while the visible lines are drawn; the pre-render
     * line reloads it from t. Incrementing during vblank as well would
     * shift the picture down by 22 lines every frame and wrap the
     * nametable. */
    if (y >= 240 || !(mask & 0x18))
        return;
    /* increment the vertical part of v, like the real PPU does */
    if ((v & 0x7000) != 0x7000) {
        v = (uint16_t)(v + 0x1000);
    } else {
        v = (uint16_t)(v & 0x0FFF);
        int cy = (v >> 5) & 0x1F;
        int nt = (v >> 10) & 3;
        if (cy == 29) {
            cy = 0;
            nt ^= 2;
        } else if (cy == 31) {
            cy = 0;
        } else {
            cy++;
        }
        v = (uint16_t)((v & 0x0C1F) | (cy << 5) | (nt << 10));
    }
}

/* The real PPU copies the temporary scroll register (t) into the current
 * address register (v) during the pre-render line. Without this, writes
 * to $2005 (games' scrolling) would never take effect. */
void ppu_latch_scroll(void)
{
    if (mask & 0x18)
        v = t;
}

void ppu_set_vblank(void)
{
    status |= 0x80;
    if (ctrl & 0x80) {
        nmi_pending = true;
        nmi_occurred = true;
    }
}

void ppu_clear_vblank(void)
{
    status &= (uint8_t)~0x80;
    nmi_occurred = false;
}

bool ppu_nmi_pending(void) { return nmi_pending; }
void ppu_clear_nmi(void)   { nmi_pending = false; }

void ppu_reset(void)
{
    ctrl = mask = status = read_buffer = 0;
    oam_addr = 0;
    v = t = 0;
    fine_x = 0;
    w = 0;
    nmi_pending = false;
    nmi_occurred = false;
    suppress_vblank = 0;
    ppu_frames = 0;
    for (int i = 0; i < 0x800; i++) vram[i] = 0;
    for (int i = 0; i < 32; i++)    pal[i] = pal_cache[i] = 0;
    for (int i = 0; i < 256; i++)   oam[i] = 0;
}
