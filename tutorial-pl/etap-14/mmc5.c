/*
 * mmc5.c — the cartridge chip itself.
 *
 * Everything in this file sits between the processor and the picture, and the
 * whole stage hangs on one idea: an address is not a place, it is a question.
 * The processor asks for $2007 and the picture chip answers; it asks for
 * $A000 and this chip answers with whichever bank it was told to show; the
 * picture chip asks for the tile at one position of one line and this chip
 * answers with a tile, a palette and a pattern bank. Stage 06 called that
 * arrangement a bus. This is the bus with the most opinionated chip on it.
 *
 * The three registers worth reading twice:
 *
 *   $5200-$5202  the split: whether it is on, which side it is on, how many
 *                columns it covers, where its picture is scrolled to, and
 *                which pattern bank it draws from.
 *   $5203/$5204  the line counter's target, and the bit that lets the
 *                interrupt through. Reading $5204 acknowledges the interrupt.
 *   $5104        what the chip's own 1 KB is for. In mode 1 it is one byte per
 *                tile of the background: two bits of palette and six bits of
 *                pattern bank, which is how two tiles on one line end up in
 *                two different palettes.
 */
#include "mmc5.h"

#define NT_SIZE     0x400       /* 960 tiles and 64 attribute bytes */
#define EXRAM_SIZE  0x400       /* the chip's own memory                */
#define CHR_BANKS   4
#define CHR_SIZE    (CHR_BANKS * 0x1000)

static uint8_t nametable[2][NT_SIZE];   /* page 0: the world, page 1: the panel */
static uint8_t exram[EXRAM_SIZE];
static uint8_t chr[CHR_SIZE];

static const uint8_t *prg;              /* the program memory, owned by main.c */
static int prg_banks;                   /* in 8 KB units */

static uint8_t prg_lo, prg_hi;          /* the windows at $8000 and $A000 */
static uint8_t exram_mode;              /* $5104 */
static uint8_t sprite_bank;             /* $5120 */
static uint8_t split_ctrl;              /* $5200 */
static uint8_t split_scroll;            /* $5201 */
static uint8_t split_bank;              /* $5202 */
static uint8_t irq_target;              /* $5203 */
static int     irq_enabled;             /* $5204, as written by the program */
static int     irq_pending;             /* $5204 bit 7, as read by it       */
static int     drawing;                 /* is the picture being drawn now?  */

void mmc5_reset(void)
{
    for (int i = 0; i < NT_SIZE; i++) {
        nametable[0][i] = 0;
        nametable[1][i] = 0;
        exram[i] = 0;
    }
    for (int i = 0; i < CHR_SIZE; i++) chr[i] = 0;

    /* Both windows start on the first bank, so a program that wants anything
     * else has to say so — which is the first thing this stage's game does. */
    prg_lo = 0;
    prg_hi = 0;
    exram_mode = 0;
    sprite_bank = 0;
    split_ctrl = 0;
    split_scroll = 0;
    split_bank = 0;
    irq_target = 0;
    irq_enabled = 0;
    irq_pending = 0;
    drawing = 0;
}

void mmc5_set_cartridge(const uint8_t *memory, int banks)
{
    prg = memory;
    prg_banks = banks;
}

void mmc5_set_drawing(int on)
{
    drawing = on;
}

/* ------------------------------------------------------------- the counter
 * The counter counts the lines of the picture that have been drawn. When the
 * count reaches the target, the chip asks for an interrupt. Target zero never
 * matches: that is how a game turns the counter off while leaving the
 * interrupt enabled, which is the chip's own rule.
 */
void mmc5_tick_line(int row)
{
    if (irq_target != 0 && row + 1 == irq_target) irq_pending = 1;
}

int mmc5_irq(void)
{
    return irq_pending && irq_enabled;
}

/* ------------------------------------------------------------ the program
 * Four 8 KB windows. The bottom two show whichever banks the program asked
 * for; the top two are wired to the last two banks of the cartridge, so the
 * reset and interrupt vectors are always mapped no matter what the program
 * does with the first two. Stage 13 met that rule; this chip keeps it.
 */
uint8_t mmc5_prg_read(uint16_t addr)
{
    int window = (addr >> 13) & 3;
    int bank;

    if (prg_banks == 0) return 0;

    if (window < 2) bank = (window == 0) ? prg_lo : prg_hi;
    else            bank = prg_banks - 4 + window;

    bank %= prg_banks;
    return prg[bank * 0x2000 + (addr & 0x1FFF)];
}

/* --------------------------------------------------------------- registers */

uint8_t mmc5_read(uint16_t addr)
{
    if (addr == 0x5204) {
        /* Bit 7 says the counter has arrived, and reading the register is how
         * the handler acknowledges it: a handler that forgets is called again
         * before the processor manages to run a single instruction. */
        uint8_t value = (uint8_t)(irq_pending ? 0x80 : 0);
        irq_pending = 0;
        return value;
    }
    /* Everything else is write-only, or is memory the processor may not read
     * in the mode this cartridge uses. */
    return 0;
}

void mmc5_write(uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0x5104:                    /* what the chip's own memory is for */
        exram_mode = (uint8_t)(value & 1);
        return;
    case 0x5116:                    /* the window at $8000 */
        prg_lo = value;
        return;
    case 0x5117:                    /* the window at $A000 */
        prg_hi = value;
        return;
    case 0x5120:                    /* which bank the sprites draw from */
        sprite_bank = value;
        return;
    case 0x5200:                    /* the split: on, side, first column */
        split_ctrl = value;
        return;
    case 0x5201:                    /* where the split's own picture starts */
        split_scroll = value;
        return;
    case 0x5202:                    /* which bank the split draws from */
        split_bank = value;
        return;
    case 0x5203:                    /* interrupt me after this many lines */
        irq_target = value;
        return;
    case 0x5204:                    /* and let the interrupt through */
        irq_enabled = (value & 0x80) != 0;
        return;
    default:
        break;
    }

    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        /* The chip's memory belongs to the picture while the picture is being
         * drawn, so the processor may write here only then. A program that
         * wants to change it half way down a frame can, because that is
         * exactly when its interrupt handler runs. */
        if (drawing) exram[addr & 0x3FF] = value;
        return;
    }
}

/* ----------------------------------------------------------- video memory
 * The chip has taken the nametables over, so the picture chip's $2000-$2FFF
 * land here. Two pages, and the split is what makes the second one visible.
 */
uint8_t mmc5_nt_read(uint16_t addr)
{
    int page = (addr >> 10) & 1;
    return nametable[page][addr & 0x3FF];
}

void mmc5_nt_write(uint16_t addr, uint8_t value)
{
    int page = (addr >> 10) & 1;
    nametable[page][addr & 0x3FF] = value;
}

/* --------------------------------------------------------------- patterns
 * A bank is 4 KB and holds 256 tiles of 16 bytes: two bytes per row, one bit
 * per pixel each, the first byte the low bit of the colour number and the
 * second the high one. That is stage 08's arrangement, unchanged.
 */
uint8_t mmc5_pattern_byte(uint8_t bank, uint8_t tile, uint8_t offset)
{
    uint32_t at = ((uint32_t)(bank % CHR_BANKS) * 0x1000)
                + ((uint32_t)tile << 4)
                + (offset & 0x0F);
    return chr[at];
}

uint8_t mmc5_sprite_bank(void)
{
    return sprite_bank;
}

/* ------------------------------------------------------------- the answer
 * One question, one answer. Without the split and without the chip's memory
 * this is a plain cartridge: the tile comes from the nametable the scroll
 * points at, the palette from that page's attribute table, the patterns from
 * bank zero. Both additions below take one of those three away.
 */
void mmc5_background(const bg_fetch_t *f, bg_tile_t *out)
{
    int page = 0, col = f->nt_col, row = f->nt_row, fine_y = f->fine_y;
    int palette;

    /* The split: a run of columns on one side of the screen, counted from the
     * left for a left split and from the right for a right one. Those columns
     * ignore the scroll the rest of the picture obeys and use the chip's own
     * vertical position instead, which is why half the screen can stand still
     * while the other half moves. */
    int threshold = split_ctrl & 0x1F;
    int in_split = (split_ctrl & 0x40) ? (f->col >= threshold)
                                       : (f->col < threshold);

    if ((split_ctrl & 0x80) && in_split) {
        page = 1;
        col = f->col & 31;
        row = ((split_scroll + f->row) >> 3) % 30;
        fine_y = (split_scroll + f->row) & 7;
    }

    if (page == 1) {
        /* The panel's palette comes from its own attribute table, and its
         * tiles from the bank the split register names. */
        uint8_t attr = nametable[1][0x3C0 + ((row >> 2) << 3) + (col >> 2)];
        palette = (attr >> (((row & 2) << 1) | (col & 2))) & 3;
        out->bank = split_bank;
    } else if (exram_mode == 1) {
        /* The chip's own memory has the last word: one byte per tile with the
         * palette in the top two bits and the pattern bank in the rest. This
         * is the whole trick behind "the palette changes while the line is
         * drawn": the palette is chosen per tile, not per 16 by 16 square. */
        uint8_t e = exram[row * 32 + col];
        palette = e >> 6;
        out->bank = (uint8_t)(e & 0x3F);
    } else {
        /* The plain path: one attribute byte per 4 by 4 tiles, one pattern
         * bank for the whole background. */
        uint8_t attr = nametable[0][0x3C0 + ((row >> 2) << 3) + (col >> 2)];
        palette = (attr >> (((row & 2) << 1) | (col & 2))) & 3;
        out->bank = 0;
    }

    out->tile = nametable[page][row * 32 + col];
    out->palette = (uint8_t)palette;
    out->fine_y = (uint8_t)fine_y;
}

/* ------------------------------------------------------------- loading door
 * Used before the first line is drawn; see mmc5.h for why it exists.
 */
void mmc5_load_nt(int page, int offset, uint8_t value)
{
    if (page < 0 || page > 1 || offset < 0 || offset >= NT_SIZE) return;
    nametable[page][offset] = value;
}

void mmc5_load_attr(int offset, uint8_t value)
{
    if (offset < 0 || offset >= EXRAM_SIZE) return;
    exram[offset] = value;
}

void mmc5_load_chr(uint8_t bank, uint16_t offset, uint8_t value)
{
    if (bank >= CHR_BANKS || offset >= 0x1000) return;
    chr[(uint32_t)bank * 0x1000 + offset] = value;
}
