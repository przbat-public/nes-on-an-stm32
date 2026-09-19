/*
 * ppu_nt_test.c — the PPU's fast nametable walk against the mapping it is
 * supposed to implement.
 *
 * MMC5 is the only mapper whose nametables the PPU cannot walk on its own
 * (the cartridge picks a source per 1 KB page). The PPU keeps its own walk
 * for every mapping that is one of the four it already knows — one-screen
 * lower/upper, horizontal, vertical — and mapper.c hands the frame back to
 * it by clearing ppu_bg_hook. That shortcut is only sound if the walk
 * reproduces the per-page mapping byte for byte, and the frame comparisons
 * cannot prove it: the wrong tile column is the 33rd fetch (or anything
 * after a coarse-X wrap), which is off screen whenever fine X is zero —
 * what every test cartridge uses.
 *
 * For every combination of mapping ($5105's four plain values), scroll
 * position, pattern table and nametable bits this renders the same
 * scanline twice:
 *
 *   hook  a reference "mapper" that answers each tile from a page table
 *         laid out exactly like $5105 (one CIRAM 1 KB page per nametable)
 *         at the address the PPU's documented v increment produces — an
 *         independent implementation of the rule;
 *   fast  the PPU's own walk with ppu_bg_hook cleared, the path real
 *         games take.
 *
 * and requires the two 256-pixel lines to be identical. While the hook
 * render runs, the test also checks the address the PPU hands the mapper
 * at every column against the reference sequence, which catches a wrong
 * coarse-X wrap even when that column's pixels happen to match.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ppu.h"
#include "nes.h"
#include "nesmem.h"

static uint8_t chr_mem[0x2000];
static uint8_t chr_fn(uint16_t a) { return chr_mem[a & 0x1FFF]; }
static void    chr_wr(uint16_t a, uint8_t v) { chr_mem[a & 0x1FFF] = v; }

/* the four $5105-style mappings under test: the register value, and the
 * CIRAM page each of the four PPU pages points at */
typedef struct { uint8_t reg; uint8_t page[4]; int mirror; const char *name; } map_t;
static const map_t maps[] = {
    { 0x00, { 0, 0, 0, 0 }, 2, "one-screen CIRAM 0 ($00)" },
    { 0x55, { 1, 1, 1, 1 }, 3, "one-screen CIRAM 1 ($55)" },
    { 0x44, { 0, 1, 0, 1 }, 1, "vertical         ($44)" },
    { 0x50, { 0, 0, 1, 1 }, 0, "horizontal       ($50)" },
};

static const uint8_t *pg[4];

/* the fetch address of tile column `col` of a line whose first tile is at
 * `la`: v increments coarse X once per fetch and toggles the horizontal
 * nametable bit when coarse X wraps. Written as a loop over the
 * documented increment rather than as an index sum, so it is an
 * independent statement of the rule the PPU's own walk must reproduce. */
static uint16_t fetch_addr(uint16_t la, int col)
{
    uint16_t a = la;
    for (int i = 0; i < col; i++) {
        if ((a & 0x1F) == 31) a = (uint16_t)((a & 0xFFE0) ^ 0x400);
        else a++;
    }
    return a;
}

static uint16_t ref_la;            /* column 0's address, from the hook  */
static unsigned long walk_bad;     /* columns where the walk disagreed   */

static uint8_t ref_hook(const ppu_bg_fetch_t *f, uint8_t *tile, uint16_t *paddr)
{
    if (f->col == 0) ref_la = f->nt_addr;
    uint16_t na = fetch_addr(ref_la, f->col);
    if (na != f->nt_addr) walk_bad++;

    *tile = pg[(na >> 10) & 3][na & 0x3FF];
    /* the attribute table from its definition rather than from the same
     * bit twiddling the PPU and mapper use: one byte per 4x4 tile block,
     * at 0x3C0 + (row >> 2) * 8 + (col >> 2), with the palette being the
     * block's (row & 2, col & 2) quadrant. Masking the row term down to
     * two bits (0x18 instead of 0x38) is a mistake that costs nothing for
     * coarse Y < 16 and the wrong palette for everything below it. */
    unsigned arow = (na >> 5) & 0x1F, acol = na & 0x1F;
    uint16_t aa = (uint16_t)(0x2000 | (na & 0xC00) | 0x3C0
                             | ((arow >> 2) << 3) | (acol >> 2));
    uint8_t attr = pg[(aa >> 10) & 3][aa & 0x3FF];
    *paddr = (uint16_t)(f->pat_base + (*tile) * 16 + f->fy);
    return (uint8_t)((attr >> (((arow & 2) << 1) | (acol & 2))) & 3);
}

static void setup(int ctrl_bits, int sx, int sy)
{
    ppu_reset();                            /* clears vram: refill it   */
    for (int i = 0; i < 0x800; i++)
        ppu_ciram()[i] = (uint8_t)(i * 7 + (i >> 4) + 1);
    /* a palette where no two entries coincide, so a wrong tile or a wrong
     * palette select cannot hide behind equal colours */
    for (int i = 0; i < 32; i++)
        ppu_write_vram((uint16_t)(0x3F00 + i), (uint8_t)((i * 5 + 3) & 0x3F));
    ppu_chr_read = chr_fn;
    ppu_chr_write = chr_wr;
    ppu_write_reg(1, 0x08);                 /* background on            */
    ppu_write_reg(0, (uint8_t)ctrl_bits);   /* pattern table + NT bits  */
    ppu_write_reg(5, (uint8_t)sx);
    ppu_write_reg(5, (uint8_t)sy);
    ppu_latch_scroll();
    ppu_end_scanline(261);                  /* line_h / line_fx         */
}

int main(void)
{
    uint8_t a[PPU_W], b[PPU_W];
    unsigned checks = 0, failed = 0, first = 1;

    for (int i = 0; i < 0x2000; i++) chr_mem[i] = (uint8_t)(i * 31 + (i >> 5));
    for (int i = 0; i < 8; i++) nes_chr[i] = chr_mem + i * 0x400;

    for (unsigned m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int page = 0; page < 4; page++)
            pg[page] = ppu_ciram() + maps[m].page[page] * 0x400;

        for (int ntx = 0; ntx < 2; ntx++)
        for (int nty = 0; nty < 2; nty++)
        for (int pat = 0; pat < 2; pat++)
        for (int cx = 0; cx < 32; cx++)
        for (int cy = 0; cy < 30; cy++)
        for (int fx = 0; fx < 8; fx++)
        for (int fy = 0; fy < 8; fy++) {
            int ctrl = pat * 0x10;
            int sx = cx * 8 + fx, sy = cy * 8 + fy;

            /* the reference: the cartridge answers every tile */
            setup(ctrl, sx, sy);
            ppu_write_reg(0, (uint8_t)(ctrl | ntx | (nty << 1)));
            ppu_latch_scroll();
            ppu_end_scanline(261);
            ppu_bg_hook = ref_hook;
            ppu_mirroring = PPU_MIRROR_MAPPER;
            ppu_render_scanline(a, 0);

            /* the fast path: the PPU's own walk */
            setup(ctrl, sx, sy);
            ppu_write_reg(0, (uint8_t)(ctrl | ntx | (nty << 1)));
            ppu_latch_scroll();
            ppu_end_scanline(261);
            ppu_bg_hook = 0;
            ppu_mirroring = maps[m].mirror;
            ppu_render_scanline(b, 0);

            checks++;
            if (memcmp(a, b, PPU_W) != 0) {
                failed++;
                if (first) {
                    int col = -1;
                    for (int x = 0; x < PPU_W; x++)
                        if (a[x] != b[x]) { col = x; break; }
                    printf("  first mismatch: %s NT bits %d%d pat %d "
                           "coarse %d,%d fine %d,%d  pixel %d: hook %02X fast %02X\n",
                           maps[m].name, nty, ntx, pat, cy, cx, fy, fx, col,
                           a[col], b[col]);
                    first = 0;
                }
            }
        }
    }

    ppu_bg_hook = 0;
    printf("ppu nametable walk: %u checks, %u failed, %lu column addresses wrong\n",
           checks, failed, walk_bad);
    return (failed || walk_bad) ? 1 : 0;
}
