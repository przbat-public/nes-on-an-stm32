/*
 * ppu.c — the NES picture chip (2C02): registers, memory and a scanline
 * renderer.
 *
 * Owns: the PPU-visible state (the $2000-$2007 registers, the nametable
 * window at $2000-$2FFF, palette RAM, OAM, the loopy scroll registers v, t,
 * fine_x and w, the status flags and the NMI line) and the code that turns a
 * line of nametables, patterns and sprites into 256 palette indices.
 *
 * What it assumes about the layer below:
 *   - pattern fetches go through ppu_chr_read()/ppu_chr_write(); mapper.c
 *     rebinds the bank behind those pointers whenever it likes, so the
 *     renderer fetches every byte through them and caches nothing;
 *   - nametables are the console's own CIRAM (vram[] here) for every mapper
 *     except MMC5, which owns them and installs the hook pointers instead;
 *   - mapper.c sets ppu_mirroring; nes.c calls ppu_render_scanline() for
 *     lines 0..239 and ppu_end_scanline() for 0..261, and hands in the row
 *     to draw into (its framebuffer row, or ppu_line when there is no
 *     display). Turning indices into colours is the display layer's job.
 *
 * Hardware quirks that shaped this file (each is also noted where it bites):
 *   - one line at a time, not one frame: games rewrite $2000/$2001/$2005/
 *     $2006 during the frame for status bars and split screens;
 *   - the horizontal scroll reloads from t at dot 257 of *every* line while
 *     the vertical half of v advances only on rendered lines, which is why
 *     line_h and line_fx exist next to v;
 *   - a line fetches 33 tile columns; the 33rd is the wrap into the other
 *     nametable and only shows up when fine X is not zero;
 *   - coarse Y wraps 29 -> 0 flipping the vertical nametable and 31 -> 0
 *     without, because rows 30/31 of the address space hold the attribute
 *     table;
 *   - OAM's Y byte is the sprite's top line minus one, so a sprite at 0
 *     first appears on line 1;
 *   - sprite 0 hit is never raised in the last column, and the overflow flag
 *     is the chip's buggy "a ninth sprite was in range";
 *   - $3F10/$14/$18/$1C mirror $3F00/$04/$08/$0C, and a $2007 read from the
 *     palette returns the value at once instead of the read buffer.
 */
#include "ppu.h"
#include "nesmem.h"      /* inline chr_read() for the pattern fetches */
#include "hal.h"

#ifdef NES_PROFILING
#define CYC_NOW() cycles_now()
#else
#define CYC_NOW() 0u
#endif

/* --------------------------- constants ---------------------------- */
/* Names for the bit positions in the PPU's registers; reading and writing
 * them as raw hex is how a mask gets misspelt into a silent picture bug. */

/* $2000-$2007: the address decoder only looks at the low three bits */
#define PPU_REG_MASK     0x07u

/* the PPU's 14-bit address space, as the renderer walks it */
#define PPU_ADDR_MASK    0x3FFFu
#define VRAM_CHR_END     0x2000u   /* $0000-$1FFF: pattern tables          */
#define NT_BASE          0x2000u   /* $2000-$2FFF: four 1 KB nametables    */
#define CIRAM_PAGE       0x0400u   /* one nametable's worth of CIRAM       */
#define NT_SPACE         0x1000u   /* all four pages together              */
#define ATTR_OFFSET      0x03C0u   /* attribute table: a page's last 64 bytes */
#define PALETTE_BASE     0x3F00u   /* $3F00-$3F1F: palette RAM, 32 bytes   */
#define PALETTE_IDX_MASK 0x1Fu
#define PALETTE_VAL_MASK 0x3Fu     /* palette RAM stores six bits per entry */

/* PPUCTRL ($2000) */
#define CTRL_NT_MASK     0x03u     /* base nametable, into t bits 10-11    */
#define CTRL_INC32       0x04u     /* $2007 steps 32: down a nametable col */
#define CTRL_SPR_1K      0x08u     /* sprite patterns from $1000           */
#define CTRL_BG_1K       0x10u     /* background patterns from $1000       */
#define CTRL_SPR16       0x20u     /* 8x16 sprites                         */
#define CTRL_NMI         0x80u     /* NMI when vblank starts               */

/* PPUMASK ($2001) */
#define MASK_BG          0x08u
#define MASK_SPR         0x10u
#define MASK_RENDERING   (MASK_BG | MASK_SPR)

/* PPUSTATUS ($2002) */
#define STATUS_OVERFLOW  0x20u
#define STATUS_SPR0_HIT  0x40u
#define STATUS_VBLANK    0x80u
#define STATUS_OPEN_BUS  0x1Fu     /* bits 0-4: the PPU's I/O latch        */

/* The loopy registers: the fields of v, and of t as $2005/$2006 write it */
#define COARSE_X_MASK    0x001Fu
#define COARSE_Y_MASK    0x03E0u
#define NT_X_BIT         0x0400u   /* horizontal nametable, toggled on wrap */
#define NT_Y_BIT         0x0800u   /* vertical nametable                    */
#define NT_BITS          (NT_X_BIT | NT_Y_BIT)
#define FINE_Y_MASK      0x7000u
#define FINE_Y_STEP      0x1000u

/* Tiles: the renderer's unit of work */
#define TILE_W           8         /* pixels, and bytes per bitplane       */
#define PLANE_BYTES      8         /* the high bitplane follows the low one */
#define TILE_BYTES       16        /* both bitplanes of one 8x8 tile       */
#define TILE_COLS        33        /* 32 visible + the prefetch after wrap */
#define PIX_MASK         0x03u     /* two bits per pixel, everywhere       */

/* Sprites */
#define SPRITE_MAX       64        /* OAM entries                          */
#define SPRITES_PER_LINE 8         /* the chip evaluates eight per line    */
#define OAM_Y            0         /* OAM byte offsets within an entry     */
#define OAM_TILE         1
#define OAM_ATTR         2
#define OAM_X            3
#define OAM_ENTRY        4
#define SPR_FLIP_V       0x80u
#define SPR_FLIP_H       0x40u
#define SPR_BEHIND       0x20u

/* Palette groups: four background palettes, then four for sprites */
#define PAL_GROUPS       8
#define PAL_ENTRIES      4
#define SPR_GROUP        4         /* sprite palette 0 is group 4 ($3F10)  */

/* The line before the first visible one: v reloads from t there and the
 * sprite flags restart, so it is where a frame begins logically. */
#define PPU_PRERENDER_LINE 261

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

/* 4-byte aligned, like the display layer's framebuffer rows: the backdrop
 * fill below stores into the row a word at a time */
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
    const uint8_t *c = &pal_cache[g * PAL_ENTRIES];
    uint8_t bd = pal_cache[0];
    for (int n = 0; n < 16; n++) {
        uint8_t p0 = (n & PIX_MASK) ? c[n & PIX_MASK] : bd;
        uint8_t p1 = ((n >> 2) & PIX_MASK) ? c[(n >> 2) & PIX_MASK] : bd;
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
static uint8_t  suppress_vblank;  /* written but never read: at -O2 the
                                   * optimiser drops the variable outright */
static int      cur_line;         /* the line being emulated now    */

/* ---------------------------- nametables -------------------------- */

/* ppu_mirroring values, as mapper.c sets them */
#define MIRROR_HORIZONTAL 0
#define MIRROR_VERTICAL   1
#define MIRROR_ONE_LOW    2        /* all four pages in CIRAM page 0       */
#define MIRROR_ONE_HIGH   3        /* all four pages in CIRAM page 1       */

/* Map a $2000-$2FFF address onto the 2 KB of CIRAM. MMC1 carts switch the
 * arrangement while the game runs, so this is a function, not a constant
 * folded into the caller. */
static uint16_t nt_index(uint16_t addr)
{
    uint16_t a = (uint16_t)((addr - NT_BASE) & (NT_SPACE - 1));
    switch (ppu_mirroring) {
    case MIRROR_VERTICAL:
        /* two 1 KB pages side by side: the offset is already the index */
        return (uint16_t)(a & (2 * CIRAM_PAGE - 1));
    case MIRROR_ONE_LOW:
        return (uint16_t)(a & (CIRAM_PAGE - 1));
    case MIRROR_ONE_HIGH:
        return (uint16_t)(CIRAM_PAGE | (a & (CIRAM_PAGE - 1)));
    case MIRROR_HORIZONTAL:
    default:
        /* horizontal: rows 0/1 come from CIRAM page 0 and rows 2/3 from
         * page 1, so the nametable's vertical bit (bit 11 of the offset)
         * becomes the CIRAM page bit (bit 10 of the index). This is also
         * the fallback for any other value a mapper leaves behind, which
         * keeps ppu_read_vram() defined even without a hook. */
        return (uint16_t)((((a >> 11) & 1) << 10) | (a & (CIRAM_PAGE - 1)));
    }
}

/* $3F10/$14/$18/$1C are mirrors of $3F00/$04/$08/$0C: the decoder ignores
 * bit 4 whenever the two low bits are zero, which is why a cartridge may
 * write the sprite backdrops at either address. */
static uint8_t palette_index(uint16_t addr)
{
    uint8_t p = (uint8_t)(addr & PALETTE_IDX_MASK);
    if ((p & 0x13) == 0x10) p &= 0x0F;
    return p;
}

uint8_t ppu_read_vram(uint16_t addr)
{
    addr &= PPU_ADDR_MASK;
    if (addr < VRAM_CHR_END) return ppu_chr_read(addr);
    if (addr < PALETTE_BASE) {
        if (ppu_nt_read_hook) return ppu_nt_read_hook(addr);
        return vram[nt_index(addr)];
    }
    return (uint8_t)(pal[palette_index(addr)] & PALETTE_VAL_MASK);
}

void ppu_write_vram(uint16_t addr, uint8_t val)
{
    addr &= PPU_ADDR_MASK;
    if (addr < VRAM_CHR_END) {
        ppu_chr_write(addr, val);
    } else if (addr < PALETTE_BASE) {
        if (ppu_nt_write_hook) ppu_nt_write_hook(addr, val);
        else                   vram[nt_index(addr)] = val;
    } else {
        uint8_t p = palette_index(addr);
        pal[p] = (uint8_t)(val & PALETTE_VAL_MASK);
        pal_cache[p] = (uint8_t)(val & PALETTE_VAL_MASK);
        /* keep the expansion tables in step; the backdrop reaches all of
         * them, the other entries only their own palette group */
        if (p == 0)
            for (int g = 0; g < PAL_GROUPS; g++) build_pair(g);
        else
            build_pair(p >> 2);
    }
}

/* ------------------------------ registers ------------------------- */

/* $2007 access advances v by one, or by 32 to walk down a nametable column
 * when PPUCTRL bit 2 is set — the reason a game sets that bit before
 * loading a column of the screen. */
static void increment_v(void)
{
    v = (uint16_t)(v + ((ctrl & CTRL_INC32) ? 32 : 1));
}

uint8_t ppu_read_reg(uint16_t addr)
{
    uint8_t r = 0;
    switch (addr & PPU_REG_MASK) {
    case 2:                                   /* PPUSTATUS */
        dbg_2002_reads++;
        /* bits 5-7 are the flags, bits 0-4 are the I/O latch: this
         * approximation shows the low bits of the $2007 read buffer */
        r = (uint8_t)((status & (STATUS_VBLANK | STATUS_SPR0_HIT | STATUS_OVERFLOW))
                      | (read_buffer & STATUS_OPEN_BUS));
        status &= (uint8_t)~STATUS_VBLANK;    /* reading it clears vblank */
        nmi_occurred = false;
        /* it also resets the $2005/$2006 write toggle, so the next write is
         * the first one — games that poll $2002 between two scroll writes
         * rely on it */
        w = 0;
        break;
    case 4:                                   /* OAMDATA */
        r = oam[oam_addr];
        break;
    case 7:                                   /* PPUDATA */
        r = read_buffer;
        read_buffer = ppu_read_vram(v);
        /* palette reads are not buffered: the value comes back now, and the
         * buffer keeps the palette byte (the chip would leave the nametable
         * byte under the palette address there, which only ever shows up in
         * the latch bits of a $2002 read) */
        if ((v & PPU_ADDR_MASK) >= PALETTE_BASE)
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
    switch (addr & PPU_REG_MASK) {
    case 0:                                   /* PPUCTRL */
        ctrl = val;
        t = (uint16_t)((t & ~(uint16_t)NT_BITS) | ((val & CTRL_NT_MASK) << 10));
        /* NMI is edge-triggered: enabling it while vblank is already
         * flagged raises one right away, which is how some games sync to
         * the frame. nmi_occurred keeps that from happening twice in one
         * vblank, because $2002 was not read in between. */
        if ((val & CTRL_NMI) && (status & STATUS_VBLANK) && !nmi_occurred) {
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
            /* first write: coarse X (bits 0-4 of t) and fine X */
            t = (uint16_t)((t & ~(uint16_t)COARSE_X_MASK) | (val >> 3));
            fine_x = (uint8_t)(val & 7);
        } else {
            /* second write: fine Y (bits 12-14 of t) and coarse Y */
            t = (uint16_t)((t & ~(uint16_t)FINE_Y_MASK) | ((val & 7) << 12));
            t = (uint16_t)((t & ~(uint16_t)COARSE_Y_MASK) | ((val & 0xF8) << 2));
        }
        w ^= 1;
        break;
    case 6:                                   /* PPUADDR */
        dbg_2006_writes++;
        if (!w) {
            t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8)); /* bits 8-13 */
        } else {
            t = (uint16_t)((t & 0xFF00) | val);
            /* the second write copies t into v: that is the latch a game
             * uses to point $2007 at something without a $2005 scroll */
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

#define SPRITE_H() ((ctrl & CTRL_SPR16) ? 2 * TILE_W : TILE_W)

typedef struct {
    uint8_t pixels[TILE_W]; /* two bitplanes, already flipped */
    uint8_t palette;        /* 0..3 */
    bool    behind;
} sprite_row_t;

/* Fetch one visible sprite's scanline once, then let the blend loop walk it
 * left to right. Doing the pattern fetch per sprite row instead of per
 * pixel is what made sprites affordable on an 80 MHz CPU; the flips are
 * applied here so the loop needs no direction of its own. */
static void sprite_fetch(int i, int row, sprite_row_t *out)
{
    uint8_t tile = oam[i * OAM_ENTRY + OAM_TILE];
    uint8_t attr = oam[i * OAM_ENTRY + OAM_ATTR];
    int r = row;
    int step;

    if (attr & SPR_FLIP_V) r = (SPRITE_H() - 1) - r;      /* flip vertical   */
    step = (attr & SPR_FLIP_H) ? -1 : 1;                  /* flip horizontal */

    uint16_t base = (ctrl & CTRL_SPR_1K) ? 0x1000 : 0x0000;
    uint16_t addr;
    if (SPRITE_H() == 2 * TILE_W) {
        /* 8x16: the tile number's bit 0 picks the pattern table instead of
         * PPUCTRL, and the two 8x8 halves are the even/odd tile pair */
        base = (uint16_t)((tile & 1) ? 0x1000 : 0x0000);
        addr = (uint16_t)(base + (tile & 0xFE) * TILE_BYTES
                          + (r >= TILE_W ? TILE_BYTES : 0) + (r & 7));
    } else {
        addr = (uint16_t)(base + tile * TILE_BYTES + (r & 7));
    }

    uint8_t lo = chr_read(addr);
    uint8_t hi = chr_read((uint16_t)(addr + PLANE_BYTES));
    int bit = (step == 1) ? 7 : 0;
    for (int k = 0; k < TILE_W; k++) {
        out->pixels[k] = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
        bit -= step;
    }
    out->palette = (uint8_t)(attr & PIX_MASK);
    out->behind  = (attr & SPR_BEHIND) != 0;
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

    if (start >= 0 && start <= PPU_W - TILE_W) {
        uint8_t *px8 = &out[start];
        put_pair(px8,     pt[pat & 0xF]);
        put_pair(px8 + 2, pt[(pat >> 4) & 0xF]);
        put_pair(px8 + 4, pt[(pat >> 8) & 0xF]);
        put_pair(px8 + 6, pt[(pat >> 12) & 0xF]);
    } else {
        uint8_t tmp[TILE_W];
        put_pair(tmp,     pt[pat & 0xF]);
        put_pair(tmp + 2, pt[(pat >> 4) & 0xF]);
        put_pair(tmp + 4, pt[(pat >> 8) & 0xF]);
        put_pair(tmp + 6, pt[(pat >> 12) & 0xF]);
        for (int bit = 0; bit < TILE_W; bit++) {
            int px = start + bit;
            if (px >= 0 && px < PPU_W)
                out[px] = tmp[bit];
        }
    }
}

/* The backdrop pre-fill is only needed when the background is off: with it
 * on the tile loop writes every pixel of the line, because the expanded
 * palette maps colour 0 to the backdrop (pair[][0]) and the edge tiles
 * write it explicitly too. That is 256 bytes a line, ~0.5 ms a frame, that
 * the background path no longer pays for. */
static void fill_backdrop(uint8_t *out, uint8_t backdrop)
{
    /* word fill, unrolled: the byte loop compiled to a memset call and the
     * plain word loop to two instructions of overhead per word */
    uint32_t bd = (uint32_t)backdrop * 0x01010101u;   /* byte in all 4 lanes */
    u32a *line32 = (u32a *)(void *)out;
    for (int i = 0; i < PPU_W / 4; i += 4) {
        line32[i + 0] = bd; line32[i + 1] = bd;
        line32[i + 2] = bd; line32[i + 3] = bd;
    }
}

/* Background when the cartridge owns the nametables (MMC5). The walk is the
 * same — one fetch per tile column — but every byte comes from wherever
 * $5105 points (CIRAM, ExRAM or the fill page), the palette may be a
 * per-tile ExRAM byte rather than an attribute-table entry, and the pattern
 * bank can change from tile to tile. All of that lives in mapper.c; the PPU
 * only supplies the position. */
static void render_bg_hook(uint8_t *out, int y)
{
    uint16_t la = (uint16_t)((v & (COARSE_Y_MASK | NT_BITS)) | line_h);
    ppu_bg_fetch_t f;
    f.pat_base = (uint16_t)((ctrl & CTRL_BG_1K) ? 0x1000 : 0x0000);
    f.fy       = (uint8_t)((v & FINE_Y_MASK) >> 12);
    f.y        = y;
    f.nt_addr  = (uint16_t)(NT_BASE | (la & (NT_SPACE - 1)));

    for (int col = 0; col < TILE_COLS; col++) {
        uint8_t tile;
        uint16_t paddr;
        f.col = (uint8_t)col;
        uint8_t palette = ppu_bg_hook(&f, &tile, &paddr);
        (void)tile;   /* the hook already turned it into paddr: the extended
                       * attribute modes can change the bank per tile */
        expand_bg_tile(out, col * TILE_W - line_fx, chr_read(paddr),
                       chr_read((uint16_t)(paddr + PLANE_BYTES)), palette);
        /* advance to the next tile column the way the PPU's v register
         * does: coarse X increments and, when it wraps, the horizontal
         * nametable bit (bit 10) toggles. A plain +1 is wrong twice
         * over — across a 1 KB boundary it walks into the attribute
         * table or into the next row instead of the other nametable,
         * which anything with a non-zero coarse X (every scrolled
         * screen) and the 33rd fetch of every line would show. */
        if ((f.nt_addr & COARSE_X_MASK) == 31)
            f.nt_addr = (uint16_t)((f.nt_addr & ~(uint16_t)COARSE_X_MASK)
                                   ^ NT_X_BIT);
        else
            f.nt_addr++;
    }
}

/* Background when the console's own CIRAM holds the nametables: the fast
 * path, which must stay a straight array walk. */
static void render_bg_direct(uint8_t *out)
{
    /* Work out the nametable position once, then walk it: tile bytes
     * inside a row are contiguous in VRAM, and a coarse-X wrap just
     * flips the 1 KB nametable bit (bit 10 of the index).
     * The horizontal half comes from the per-line reload of t, the
     * vertical half (coarse Y, NT Y, fine Y) from v. */
    uint16_t la = (uint16_t)((v & (COARSE_Y_MASK | NT_BITS)) | line_h);
    int cx  = la & COARSE_X_MASK;
    int cy  = (v & COARSE_Y_MASK) >> 5;
    int fy  = (v & FINE_Y_MASK) >> 12;
    uint16_t pat_base = (ctrl & CTRL_BG_1K) ? 0x1000 : 0x0000;
    uint16_t nt_idx = nt_index((uint16_t)(NT_BASE | (la & (NT_SPACE - 1))));
    int at_row = ATTR_OFFSET | ((cy >> 2) << 3);

    for (int col = 0; col < TILE_COLS; col++) {
        uint8_t tile = vram[nt_idx];
        /* the attribute table follows the nametable select bit, so it
         * must be recomputed after a coarse-X wrap */
        uint8_t attr = vram[at_row | (nt_idx & NT_X_BIT) | (cx >> 2)];
        /* each attribute byte covers a 4x4 tile block, two bits per 2x2
         * quadrant: bit 1 is the right half of the block, bit 2 the
         * bottom half */
        uint8_t palette = (uint8_t)((attr >> (((cy & 2) << 1) | (cx & 2)))
                                    & PIX_MASK);

        uint16_t paddr = (uint16_t)(pat_base + tile * TILE_BYTES + fy);
        expand_bg_tile(out, col * TILE_W - line_fx, chr_read(paddr),
                       chr_read((uint16_t)(paddr + PLANE_BYTES)), palette);

        /* advance to the next tile column: inside a row the VRAM
         * index just increments; column 32 is the one the PPU fetches
         * after the horizontal wrap, i.e. the same row with the
         * horizontal nametable bit toggled. Only the vertical
         * arrangement puts that bit into the CIRAM index (see
         * nt_index); the others mirror it back onto the same byte.
         * Getting this wrong shows up as one wrong tile column at the
         * right edge whenever fine X is not zero, so the flip is done
         * through the mirroring rather than by toggling the index. */
        if (cx == 31) {
            nt_idx = (uint16_t)(nt_idx - 31);
            if (ppu_mirroring == MIRROR_VERTICAL) nt_idx ^= CIRAM_PAGE;
        } else {
            nt_idx++;
        }
        cx = (cx + 1) & 31;
    }
}

/* Blend the sprites of one scanline over the background. Only the first
 * eight sprites whose Y range covers the line are drawn; the ninth only
 * raises the overflow flag. */
static void render_sprites(uint8_t *out, int y, bool bg_on, uint8_t backdrop)
{
    int h = SPRITE_H();
    int count = 0;
    for (int i = 0; i < SPRITE_MAX; i++) {
        /* OAM's Y byte is the top line minus one, so a sprite at $00 first
         * shows on line 1 — the hardware quirk every game's sprite table
         * is written around */
        int sy = oam[i * OAM_ENTRY + OAM_Y] + 1;
        if (y < sy || y >= sy + h)
            continue;
        count++;
        if (count > SPRITES_PER_LINE) {
            /* the real PPU raises the overflow flag (buggy, but the
             * flag is what games poll) */
            status |= STATUS_OVERFLOW;
            break;
        }
        int sx = oam[i * OAM_ENTRY + OAM_X];
        if (sx >= PPU_W)              /* the X byte is the left edge; there
                                       * is no wrap onto the next line */
            continue;

        sprite_row_t spr;
        sprite_fetch(i, y - sy, &spr);
        const uint8_t *pc = &pal_cache[(SPR_GROUP + spr.palette) * PAL_ENTRIES];

        int n = PPU_W - sx;
        if (n > TILE_W) n = TILE_W;
        for (int c = 0; c < n; c++) {
            uint8_t pix = spr.pixels[c];
            if (pix == 0)
                continue;                  /* colour 0 is transparent */
            int px = sx + c;
            /* A background pixel counts as opaque when it is not the backdrop
             * colour. A palette entry that happens to equal the backdrop
             * therefore reads as transparent here, where the chip compares
             * the pixel's palette *index* instead. */
            bool bg_opaque = bg_on && out[px] != backdrop;

            /* Sprite 0 hit: only sprite 0, only where it and the background
             * are both opaque, and never in the last column — the chip does
             * not compare there. Games use the flag as a beam counter, so a
             * wrong column moves their split screen. */
            if (i == 0 && bg_opaque && px < PPU_W - 1) {
                if (!(status & STATUS_SPR0_HIT)) dbg_sprite0_hits++;
                status |= STATUS_SPR0_HIT;
            }

            if (spr.behind && bg_opaque)
                continue;
            /* Sprites blend in OAM order, so where two overlap the *last* one
             * drawn wins; the chip gives priority to the lowest OAM index.
             * Fixing that would repaint every overlapping sprite pair, so it
             * stays a documented divergence instead of a silent one. */
            out[px] = pc[pix];
        }
    }
}

void ppu_render_scanline(uint8_t *out, int y)
{
    uint32_t t0 = CYC_NOW(), t1;
    uint8_t backdrop = pal_cache[0];

    bool bg_on = (mask & MASK_BG) != 0;
    bool sp_on = (mask & MASK_SPR) != 0;

    if (!bg_on)
        fill_backdrop(out, backdrop);

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
    if (bg_on && ppu_bg_hook)
        render_bg_hook(out, y);
    else if (bg_on)
        render_bg_direct(out);

    /* ---------------- sprites ---------------- */
    t1 = CYC_NOW();
    acc_bg += t1 - t0;
    t0 = t1;
    /* MMC5 keeps the sprite CHR banks in a different set of registers
     * ($5120-$5127) from the background's ($5128-$512B), so the renderer
     * hands the pass boundary to the mapper before fetching sprites. */
    if (ppu_chr_spr_hook)
        ppu_chr_spr_hook();
    if (sp_on)
        render_sprites(out, y, bg_on, backdrop);
    acc_spr += CYC_NOW() - t0;
}

/* ------------------------- per-line scrolling --------------------- */

volatile uint32_t dbg_mask, dbg_ctrl, dbg_v, dbg_t;
volatile uint32_t ppu_dbg_ctrl, ppu_dbg_mask;
volatile uint32_t dbg_2002_reads, dbg_sprite0_hits, dbg_2007_writes, dbg_2006_writes;

/* Increment the vertical half of v the way the chip's address counter does:
 * fine Y steps through the eight lines of a tile row, and its wrap bumps
 * coarse Y. Rows 30 and 31 of the address space hold the attribute table,
 * hence the two odd cases — 29 wraps back to 0 and flips the vertical
 * nametable, 31 wraps to 0 without flipping. Getting either wrong shifts
 * the picture by whole nametable rows or scrolls into attribute data. */
static void increment_v_vertical(void)
{
    if ((v & FINE_Y_MASK) != FINE_Y_MASK) {
        v = (uint16_t)(v + FINE_Y_STEP);
        return;
    }
    v = (uint16_t)(v & (COARSE_X_MASK | COARSE_Y_MASK | NT_BITS));
    int cy = (v & COARSE_Y_MASK) >> 5;
    int nt = (v & NT_BITS) >> 10;
    if (cy == 29) {
        cy = 0;
        nt ^= 2;                  /* 29 -> 0 crosses into the other half */
    } else if (cy == 31) {
        cy = 0;                   /* row 31 is attribute data, not a row */
    } else {
        cy++;
    }
    v = (uint16_t)((v & (COARSE_X_MASK | NT_BITS)) | (cy << 5) | (nt << 10));
}

void ppu_end_scanline(int y)
{
    cur_line = y;
    /* The pre-render line is the frame boundary: the game has written the
     * scroll for the whole frame by then, which is why the debug snapshot
     * (tools/swd.py) is taken there. */
    if (y == PPU_PRERENDER_LINE) {
        dbg_mask = mask; dbg_ctrl = ctrl; dbg_v = v; dbg_t = t;
        ppu_dbg_ctrl = ctrl; ppu_dbg_mask = mask;
    }

    /* dot 257 of this line: the horizontal scroll for the next one is
     * reloaded from t (coarse X and the horizontal nametable bit), and the
     * fine X latches with it. Do it before the early return: vblank lines
     * reload too, and the first visible line must see the values the game
     * wrote during vblank. */
    line_h  = (uint16_t)(t & (COARSE_X_MASK | NT_X_BIT));
    line_fx = fine_x;

    /* v advances only while the visible lines are drawn; the pre-render
     * line reloads it from t. Incrementing during vblank as well would
     * shift the picture down by 22 lines every frame and wrap the
     * nametable. */
    if (y >= PPU_H || !(mask & MASK_RENDERING))
        return;
    increment_v_vertical();
}

/* The real PPU copies the temporary scroll register (t) into the current
 * address register (v) during the pre-render line. Without this, writes
 * to $2005 (games' scrolling) would never take effect. */
void ppu_latch_scroll(void)
{
    if (mask & MASK_RENDERING)
        v = t;
}

void ppu_set_vblank(void)
{
    status |= STATUS_VBLANK;
    if (ctrl & CTRL_NMI) {
        nmi_pending = true;
        nmi_occurred = true;
    }
}

void ppu_clear_vblank(void)
{
    status &= (uint8_t)~STATUS_VBLANK;
    nmi_occurred = false;
}

/* The sprite flags live in the same status byte and, like vblank, are
 * cleared once per frame — at the pre-render line, not by reading $2002.
 * Games use sprite 0 as a beam counter: poll $2002 bit 6 until it clears,
 * then until it sets again, and that lands them at a known scanline for a
 * split screen. Leaving the flag sticky hangs them in the first loop. */
void ppu_clear_sprite_flags(void)
{
    status &= (uint8_t)~(STATUS_SPR0_HIT | STATUS_OVERFLOW);
}

bool ppu_nmi_pending(void) { return nmi_pending; }
void ppu_clear_nmi(void)   { nmi_pending = false; }

/* The MMC3 scanline counter is clocked by the PPU's pattern fetches, so it
 * only ticks while the picture is actually being drawn. */
bool ppu_rendering_enabled(void) { return (mask & MASK_RENDERING) != 0; }

/* The line the PPU is in the middle of (set at the end of each one, so it
 * is the line the CPU is running during). MMC5 needs it for the two rules
 * that follow PPU /RD activity: ExRAM is write-only-while-rendering in
 * $5104 modes %00/%01, and the "in frame" status bit follows it. */
bool ppu_visible(void)
{
    return cur_line < PPU_H && (mask & MASK_RENDERING) != 0;
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
    /* the tables are not optional state: the renderer reads them before any
     * palette write happens (a game may never touch $3F00), so reset has to
     * build them for an all-backdrop palette */
    build_sprd();
    for (int g = 0; g < PAL_GROUPS; g++) build_pair(g);
}
