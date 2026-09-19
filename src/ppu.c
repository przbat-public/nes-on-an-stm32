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
#include "hal.h"

#ifdef NES_PROFILING
#define CYC_NOW() cycles_now()
#else
#define CYC_NOW() 0u
#endif

/* where the renderer's time goes. The accumulators are plain statics (the
 * += happens once per line, the publish once per frame) and the dbg_*
 * values are the last *complete* frame, so one SWD read is self-consistent
 * even if the scene changes under it. */
volatile uint32_t dbg_cyc_fill, dbg_cyc_bg, dbg_cyc_spr;
static uint32_t acc_fill, acc_bg, acc_spr;

/* how many background tiles in the last frame expanded to nothing (both
 * pattern bytes zero): the quickest way to tell "the nametable is empty"
 * from "the CHR bank is wrong" when a cartridge draws a blank screen */
volatile uint32_t dbg_bg_tiles, dbg_bg_zero;
static uint32_t acc_tiles, acc_zero;

void ppu_dbg_frame(void)
{
    dbg_cyc_fill = acc_fill; acc_fill = 0;
    dbg_cyc_bg   = acc_bg;   acc_bg   = 0;
    dbg_cyc_spr  = acc_spr;  acc_spr  = 0;
    dbg_bg_tiles = acc_tiles; acc_tiles = 0;
    dbg_bg_zero  = acc_zero;  acc_zero  = 0;
}

uint8_t  ppu_line[PPU_W] __attribute__((aligned(4)));
uint32_t ppu_frames;

uint8_t (*ppu_chr_read)(uint16_t addr);
void    (*ppu_chr_write)(uint16_t addr, uint8_t v);
int      ppu_mirroring;

/* installed by the MMC5 (see ppu.h); NULL for every other mapper, which
 * keeps the PPU's own nametable path as the fast one */
uint8_t (*ppu_bg_hook)(const ppu_bg_fetch_t *f, uint8_t *tile, uint16_t *paddr);
void    (*ppu_chr_bg_hook)(void);
void    (*ppu_chr_spr_hook)(void);
uint8_t (*ppu_nt_read_hook)(uint16_t addr);
void    (*ppu_nt_write_hook)(uint16_t addr, uint8_t v);

/* ------------------------------- state ---------------------------- */

static uint8_t  vram[0x800];      /* 2 KB nametables (mirrored)     */
static uint8_t  pal[32];          /* palette RAM                    */
static uint8_t  pal_cache[32];    /* same values, masked: the renderer
                                   * reads these 61k times per frame
                                   * instead of calling a function      */
static uint8_t  oam[256];         /* sprite memory                  */
static uint8_t  oam_addr;

/* ------------------------- tile expansion ------------------------- */
/*
 * The background used to be expanded one pixel at a time:
 *
 *     for (bit = 0; bit < 8; bit++) {
 *         pix = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
 *         if (pix) out[bit] = (pix == 1) ? c1 : (pix == 2) ? c2 : c3;
 *     }
 *
 * which is ~19 instructions and two data-dependent branches per pixel —
 * it was 42% of a whole frame. Instead the eight pixels are packed into
 * one 16-bit word (two bits per pixel) with a bit spread, and turned into
 * eight palette bytes two pixels at a time through a 16-entry table per
 * palette group. The zero pixels need no special case: ppu_line is
 * pre-filled with the backdrop colour, and writing that value back is
 * exactly what leaving the pixel alone used to do.
 *
 *   sprd[x] bit-reverses a pattern byte so pixel k is bit k of it, then
 *           moves bit k to bit 2k, leaving the gap the second bitplane
 *           goes into: two of them OR together into a 2-bit-per-pixel
 *           pattern word. Done with shifts it is a 22-instruction serial
 *           chain per tile column, which is what a 256-entry table avoids.
 *   pair[g][nibble] is the two output bytes for the two pixels of that
 *           nibble, for palette group g (rebuilt when palette RAM changes)
 */
static uint16_t pair[8][16];
static uint16_t sprd[256];

/* group g is pal_cache[4g..4g+3]: background palettes 0-3, sprite 4-7.
 *
 * Pixel value 0 of *every* background palette shows the universal backdrop
 * at $3F00 (pal_cache[0]) — the colour written to $3F04/$3F08/$3F0C is not
 * what a zero pixel displays. Getting this wrong is invisible on the test
 * cartridges, which write the same colour to all four backdrop entries;
 * tools/ppu_expand_test.c compares this against the per-pixel loop for a
 * palette RAM where nothing coincides. */
static void build_pair(int g)
{
    const uint8_t *c = &pal_cache[g * 4];
    uint8_t bd = pal_cache[0];
    for (int n = 0; n < 16; n++) {
        uint8_t p0 = (n & 3) ? c[n & 3] : bd;
        uint8_t p1 = ((n >> 2) & 3) ? c[(n >> 2) & 3] : bd;
        pair[g][n] = (uint16_t)(p0 | (p1 << 8));
    }
}

static void build_sprd(void)
{
    for (int x = 0; x < 256; x++) {
        uint32_t r = 0;
        for (int k = 0; k < 8; k++)
            if (x & (1 << k)) r |= 1u << (2 * (7 - k));
        sprd[x] = (uint16_t)r;
    }
}

typedef uint32_t __attribute__((may_alias)) u32a;

/* two pixels, one 16-bit store; the address is not always 4-byte aligned
 * (fine_x shifts the whole line by up to 7), which the M4 allows */
typedef struct { uint16_t h; } __attribute__((packed, may_alias)) u16u;

static inline void put_pair(uint8_t *out, uint16_t v)
{
    ((u16u *)(void *)out)->h = v;
}


static uint8_t  ctrl;             /* $2000 */
static uint8_t  mask;             /* $2001 */
static uint8_t  status;           /* $2002 */
static uint8_t  read_buffer;      /* $2007 buffered read            */

/* loopy scroll registers */
static uint16_t v, t;             /* current / temporary VRAM address */
static uint8_t  fine_x;
static uint8_t  w;                /* first/second write toggle      */

/* The horizontal scroll is not a once-a-frame value. The real PPU reloads
 * coarse X and the horizontal nametable bit from t at dot 257 of *every*
 * scanline (and the fine X applies to the next line's fetches), which is
 * what makes split screens possible: a game writes $2005 right after the
 * sprite-0 hit so the playfield gets a different scroll than the status
 * bar above it. Taking the whole position from v once per frame made such
 * games draw the playfield with the status bar's coarse X while the fine X
 * still changed — the picture jittered inside an 8-pixel window instead of
 * scrolling. These two hold what the next line will start with. */
static uint16_t line_h;           /* coarse X (bits 0-4) + NT X (bit 10) */
static uint8_t  line_fx;          /* fine X                             */

static bool     nmi_pending;
static bool     nmi_occurred;     /* for the $2002 read behaviour   */
static uint8_t  suppress_vblank;
static int      cur_line;         /* the line being emulated now    */

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
    if (addr < 0x3F00) {
        if (ppu_nt_read_hook) return ppu_nt_read_hook(addr);
        return vram[nt_index(addr)];
    }
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
        if (ppu_nt_write_hook) ppu_nt_write_hook(addr, val);
        else                   vram[nt_index(addr)] = val;
    } else {
        uint8_t p = (uint8_t)(addr & 0x1F);
        if ((p & 0x13) == 0x10) p &= 0x0F;
        pal[p] = (uint8_t)(val & 0x3F);
        pal_cache[p] = (uint8_t)(val & 0x3F);
        /* keep the expansion tables in step; the backdrop reaches all of
         * them, the other entries only their own palette group */
        if (p == 0)
            for (int g = 0; g < 8; g++) build_pair(g);
        else
            build_pair(p >> 2);
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
        dbg_2002_reads++;
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
        dbg_2006_writes++;
        if (!w) {
            t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8));
        } else {
            t = (uint16_t)((t & 0xFF00) | val);
            v = t;
        }
        w ^= 1;
        break;
    case 7:                                   /* PPUDATA */
        dbg_2007_writes++;
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

/* Expand one background tile's two pattern bytes into eight palette bytes
 * at `start` (which may be off either end of the line by up to seven
 * pixels). The common case — the whole tile is on screen — is four 16-bit
 * stores; the first and last column of the line go through the same
 * tables into a scratch row and copy the visible part, which is also what
 * keeps colour 0 mapped to the backdrop. Both the PPU's own nametable
 * walk and the mapper's (MMC5) use this, so the two paths cannot drift. */
static inline void expand_bg_tile(uint8_t *out, int start,
                                  uint8_t lo, uint8_t hi, uint8_t palette)
{
    const uint16_t *pt = pair[palette];
    uint32_t pat = sprd[lo] | ((uint32_t)sprd[hi] << 1);

    acc_tiles++;
    if (!pat) acc_zero++;

    if (start >= 0 && start <= PPU_W - 8) {
        uint8_t *px8 = &out[start];
        put_pair(px8,     pt[pat & 0xF]);
        put_pair(px8 + 2, pt[(pat >> 4) & 0xF]);
        put_pair(px8 + 4, pt[(pat >> 8) & 0xF]);
        put_pair(px8 + 6, pt[(pat >> 12) & 0xF]);
    } else {
        uint8_t tmp[8];
        put_pair(tmp,     pt[pat & 0xF]);
        put_pair(tmp + 2, pt[(pat >> 4) & 0xF]);
        put_pair(tmp + 4, pt[(pat >> 8) & 0xF]);
        put_pair(tmp + 6, pt[(pat >> 12) & 0xF]);
        for (int bit = 0; bit < 8; bit++) {
            int px = start + bit;
            if (px >= 0 && px < PPU_W)
                out[px] = tmp[bit];
        }
    }
}

void ppu_render_scanline(uint8_t *out, int y)
{
    uint32_t t0 = CYC_NOW(), t1;
    uint8_t backdrop = pal_cache[0];

    bool bg_on = (mask & 0x08) != 0;
    bool sp_on = (mask & 0x10) != 0;

    /* The backdrop pre-fill is only needed when the background is off: with
     * it on the tile loop below writes every pixel of the line, because the
     * expanded palette maps colour 0 to the backdrop (pair[][0]) and the
     * edge tiles write it explicitly too. That is 256 bytes a line, ~0.5 ms
     * a frame, that the background path no longer pays for. */
    if (!bg_on) {
        /* word fill, unrolled: the byte loop compiled to a memset call and
         * the plain word loop to two instructions of overhead per word */
        uint32_t bd = (uint32_t)backdrop * 0x01010101u;
        u32a *line32 = (u32a *)(void *)out;
        for (int i = 0; i < PPU_W / 4; i += 4) {
            line32[i + 0] = bd; line32[i + 1] = bd;
            line32[i + 2] = bd; line32[i + 3] = bd;
        }
    }

    t1 = CYC_NOW();
    acc_fill += t1 - t0;
    t0 = t1;

    /* MMC5 keeps the background CHR banks ($5128-$512B when 8x16 sprites
     * are on, $5120-$5127 otherwise) apart from the sprite ones, so the
     * pass boundary is where nes_chr[] has to be rebound — whether or not
     * the nametable hook below is in use. */
    if (ppu_chr_bg_hook)
        ppu_chr_bg_hook();

    /* ---------------- background ---------------- */
    if (bg_on && ppu_bg_hook) {
        /* The cartridge owns the nametables (MMC5). The walk is the same —
         * one fetch per tile column — but every byte comes from wherever
         * $5105 points (CIRAM, ExRAM or the fill page), the palette may be
         * a per-tile ExRAM byte rather than an attribute-table entry, and
         * the pattern bank can change from tile to tile. All of that lives
         * in mapper.c; the PPU only supplies the position. */
        uint16_t la = (uint16_t)((v & 0x0BE0) | line_h);
        ppu_bg_fetch_t f;
        f.pat_base = (uint16_t)((ctrl & 0x10) ? 0x1000 : 0x0000);
        f.fy       = (uint8_t)((v >> 12) & 7);
        f.y        = y;
        f.nt_addr  = (uint16_t)(0x2000 | (la & 0x0FFF));

        for (int col = 0; col < 33; col++) {
            uint8_t tile;
            uint16_t paddr;
            f.col = (uint8_t)col;
            uint8_t palette = ppu_bg_hook(&f, &tile, &paddr);
            (void)tile;
            expand_bg_tile(out, col * 8 - line_fx, chr_read(paddr),
                           chr_read((uint16_t)(paddr + 8)), palette);
            /* advance to the next tile column the way the PPU's v register
             * does: coarse X increments and, when it wraps, the horizontal
             * nametable bit (bit 10) toggles. A plain +1 is wrong twice
             * over — across a 1 KB boundary it walks into the attribute
             * table or into the next row instead of the other nametable,
             * which anything with a non-zero coarse X (every scrolled
             * screen) and the 33rd fetch of every line would show. */
            if ((f.nt_addr & 0x1F) == 31)
                f.nt_addr = (uint16_t)((f.nt_addr & 0xFFE0) ^ 0x400);
            else
                f.nt_addr++;
        }
    } else if (bg_on) {
        /* Work out the nametable position once, then walk it: tile bytes
         * inside a row are contiguous in VRAM, and a coarse-X wrap just
         * flips the 1 KB nametable bit (bit 10 of the index).
         * The horizontal half comes from the per-line reload of t, the
         * vertical half (coarse Y, NT Y, fine Y) from v. */
        uint16_t la = (uint16_t)((v & 0x0BE0) | line_h);
        int cx  = la & 0x1F;
        int cy  = (v >> 5) & 0x1F;
        int fy  = (v >> 12) & 7;
        uint16_t pat_base = (ctrl & 0x10) ? 0x1000 : 0x0000;
        uint16_t nt_idx = nt_index((uint16_t)(0x2000 | (la & 0x0FFF)));
        int at_row = 0x3C0 | ((cy >> 2) << 3);

        for (int col = 0; col < 33; col++) {
            uint8_t tile = vram[nt_idx];
            /* the attribute table follows the nametable select bit, so it
             * must be recomputed after a coarse-X wrap */
            uint8_t attr = vram[at_row | (nt_idx & 0x400) | (cx >> 2)];
            uint8_t palette = (uint8_t)((attr >> (((cy & 2) << 1) | (cx & 2))) & 3);

            uint16_t paddr = (uint16_t)(pat_base + tile * 16 + fy);
            expand_bg_tile(out, col * 8 - line_fx, chr_read(paddr),
                           chr_read((uint16_t)(paddr + 8)), palette);

            /* advance to the next tile column: inside a row the VRAM
             * index just increments; column 32 is the one the PPU fetches
             * after the horizontal wrap, i.e. the same row with the
             * horizontal nametable bit toggled — which is the same CIRAM
             * byte only in the two arrangements that put that bit in the
             * index. Getting this wrong shows up as one wrong tile column
             * at the right edge whenever fine X is not zero, so the flip
             * is done through the mirroring rather than by toggling the
             * index: vertical (a & 0x7FF) is the only mode where the
             * horizontal bit selects the 1 KB page. */
            if (cx == 31) {
                nt_idx = (uint16_t)(nt_idx - 31);
                if (ppu_mirroring == 1) nt_idx ^= 0x400;
            } else {
                nt_idx++;
            }
            cx = (cx + 1) & 31;
        }
    }

    /* ---------------- sprites ---------------- */
    t1 = CYC_NOW();
    acc_bg += t1 - t0;
    t0 = t1;
    /* MMC5 keeps the sprite CHR banks in a different set of registers
     * ($5120-$5127) from the background's ($5128-$512B), so the renderer
     * hands the pass boundary to the mapper before fetching sprites. */
    if (ppu_chr_spr_hook)
        ppu_chr_spr_hook();
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
                bool bg_opaque = bg_on && out[px] != backdrop;

                if (i == 0 && bg_opaque && px < 255) {
                    if (!(status & 0x40)) dbg_sprite0_hits++;
                    status |= 0x40;                 /* sprite 0 hit */
                }

                if (spr.behind && bg_opaque)
                    continue;
                out[px] = pc[pix];
            }
        }
    }
    acc_spr += CYC_NOW() - t0;
}

/* ------------------------- per-line scrolling --------------------- */

volatile uint32_t dbg_mask, dbg_ctrl, dbg_v, dbg_t;
volatile uint32_t ppu_dbg_ctrl, ppu_dbg_mask;
volatile uint32_t dbg_2002_reads, dbg_sprite0_hits, dbg_2007_writes, dbg_2006_writes;

void ppu_end_scanline(int y)
{
    cur_line = y;
    if (y == 261) { dbg_mask = mask; dbg_ctrl = ctrl; dbg_v = v; dbg_t = t;
                    ppu_dbg_ctrl = ctrl; ppu_dbg_mask = mask; }

    /* dot 257 of this line: the horizontal scroll for the next one is
     * reloaded from t (coarse X and the horizontal nametable bit), and the
     * fine X latches with it. Do it before the early return: vblank lines
     * reload too, and the first visible line must see the values the game
     * wrote during vblank. */
    line_h  = (uint16_t)(t & 0x041F);
    line_fx = fine_x;

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

/* The sprite flags live in the same status byte and, like vblank, are
 * cleared once per frame — at the pre-render line, not by reading $2002.
 * Games use sprite 0 as a beam counter: poll $2002 bit 6 until it clears,
 * then until it sets again, and that lands them at a known scanline for a
 * split screen. Leaving the flag sticky hangs them in the first loop. */
void ppu_clear_sprite_flags(void)
{
    status &= (uint8_t)~(0x40 | 0x20);
}

bool ppu_nmi_pending(void) { return nmi_pending; }
void ppu_clear_nmi(void)   { nmi_pending = false; }

/* The MMC3 scanline counter is clocked by the PPU's pattern fetches, so it
 * only ticks while the picture is actually being drawn. */
bool ppu_rendering_enabled(void) { return (mask & 0x18) != 0; }

/* The line the PPU is in the middle of (set at the end of each one, so it
 * is the line the CPU is running during). MMC5 needs it for the two rules
 * that follow PPU /RD activity: ExRAM is write-only-while-rendering in
 * $5104 modes %00/%01, and the "in frame" status bit follows it. */
bool ppu_visible(void)
{
    return cur_line < PPU_H && (mask & 0x18) != 0;
}

uint8_t *ppu_ciram(void) { return vram; }

void ppu_reset(void)
{
    ctrl = mask = status = read_buffer = 0;
    oam_addr = 0;
    v = t = 0;
    fine_x = 0;
    line_h = 0;
    line_fx = 0;
    w = 0;
    cur_line = PPU_H;             /* not rendering until the first line */
    nmi_pending = false;
    nmi_occurred = false;
    suppress_vblank = 0;
    ppu_frames = 0;
    for (int i = 0; i < 0x800; i++) vram[i] = 0;
    for (int i = 0; i < 32; i++)    pal[i] = pal_cache[i] = 0;
    for (int i = 0; i < 256; i++)   oam[i] = 0;
    build_sprd();
    for (int g = 0; g < 8; g++) build_pair(g);
}
