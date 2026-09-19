/*
 * ppu.c — one line at a time, and a question to the cartridge for every tile.
 *
 * The picture chip holds four things: the palette, the sprites, the scroll,
 * and a small set of registers the processor writes. It holds no picture at
 * all. Every line it draws is assembled from tiles it fetches as it goes, and
 * that is the part worth watching in this file: the fetch is a call into the
 * cartridge, so the cartridge sees the line being built and answers tile by
 * tile. A cartridge that answers "different palette" for one tile and
 * "different nametable" for the next eight columns is a cartridge that can
 * change the picture in the middle of a line.
 */
#include "ppu.h"
#include "mmc5.h"

/* The picture, one byte per pixel. It starts as one flat colour: the colour
 * numbered zero, which is what the array's initialiser says. */
uint8_t ppu_framebuffer[PPU_H][PPU_W] = { { 0 } };

static uint8_t palette_ram[32];     /* four palettes for the picture, four for sprites */
static uint8_t oam[256];            /* the sprite list: four bytes each               */

static uint8_t  oam_addr;
static uint16_t address;            /* where the next $2007 write goes */
static int      address_high;       /* the two-write latch of $2006    */
static int      scroll_write;       /* the two-write latch of $2005    */
static uint8_t  scroll_x, scroll_y; /* the position in use for this line  */
static uint8_t  next_x, next_y;     /* what the program wrote for the next */
static uint8_t  mask;
static int      vblank;

void ppu_reset(void)
{
    for (int i = 0; i < 32; i++)  palette_ram[i] = 0;
    for (int i = 0; i < 256; i++) oam[i] = 0;
    oam_addr = 0;
    address = 0;
    address_high = 0;
    scroll_write = 0;
    scroll_x = scroll_y = 0;
    next_x = next_y = 0;
    mask = 0;
    vblank = 0;
}

void ppu_set_vblank(int on)
{
    vblank = on;
}

/* ------------------------------------------------------------- one tile
 * A tile is eight pixels, and each pixel is two bits out of two bytes: the
 * first byte holds the low bit of every pixel of the row, the second the high
 * bit. Bit 7 is the leftmost pixel, which is why the shift counts down.
 */
static int pattern_pixel(uint8_t bank, uint8_t tile, int row, int x)
{
    uint8_t low  = mmc5_pattern_byte(bank, tile, (uint8_t)(row * 2));
    uint8_t high = mmc5_pattern_byte(bank, tile, (uint8_t)(row * 2 + 1));
    return (int)(((low >> (7 - x)) & 1) | (((high >> (7 - x)) & 1) << 1));
}

static uint8_t background_colour(int pixel, int palette)
{
    /* Colour zero is not the tile's colour: it is the one colour the whole
     * picture shares. That is what lets a tile show what is behind it. */
    if (pixel == 0) return palette_ram[0];
    return palette_ram[palette * 4 + pixel];
}

static void draw_sprites(int row)
{
    /* Later sprites go first, so the earlier one in the list wins the pixels
     * they share. That is the console's rule and the reason a game can put a
     * character in front of a background sprite by giving it a lower number. */
    for (int i = 63; i >= 0; i--) {
        int y = oam[i * 4 + 0];
        int tile = oam[i * 4 + 1];
        int attr = oam[i * 4 + 2];
        int x = oam[i * 4 + 3];

        if (row < y || row >= y + 8) continue;

        for (int j = 0; j < 8; j++) {
            int pixel = pattern_pixel(mmc5_sprite_bank(), (uint8_t)tile,
                                      row - y, j);
            int at = x + j;
            if (pixel == 0 || at < 0 || at >= PPU_W) continue;
            ppu_framebuffer[row][at] = palette_ram[16 + (attr & 3) * 4 + pixel];
        }
    }
}

/* -------------------------------------------------------------- one line */
void ppu_render_row(int row)
{
    uint8_t line[33 * 8];       /* 33 tiles, so the last one can spill over */

    /* A write made while the previous line was being drawn lands here, at the
     * start of this one. Stage 12 met that rule as "a scroll written between
     * two lines takes effect on the next line"; it is the same rule that lets
     * an interrupt handler change the bottom half of a frame. */
    scroll_x = next_x;
    scroll_y = next_y;

    for (int col = 0; col < 33; col++) {
        bg_fetch_t ask;
        bg_tile_t answer = { 0, 0, 0, 0 };

        ask.col = col;
        ask.row = row;
        ask.nt_col = ((scroll_x >> 3) + col) & 31;
        ask.nt_row = ((scroll_y + row) >> 3) % 30;
        ask.fine_y = (scroll_y + row) & 7;
        mmc5_background(&ask, &answer);

        for (int x = 0; x < 8; x++) {
            int pixel = (mask & 0x08)
                      ? pattern_pixel(answer.bank, answer.tile,
                                      answer.fine_y, x)
                      : 0;
            line[col * 8 + x] = background_colour(pixel, answer.palette);
        }
    }

    /* The fine horizontal scroll belongs to this chip and not to the
     * cartridge: one shift moves the whole line, the left half as much as the
     * right. A cartridge can therefore say what to draw in a column, but not
     * where inside it. This stage's game scrolls vertically and leaves this at
     * zero, which keeps that limitation out of the way. */
    for (int x = 0; x < PPU_W; x++)
        ppu_framebuffer[row][x] = line[x + (scroll_x & 7)];

    if (mask & 0x10) draw_sprites(row);
}

/* ---------------------------------------------------------------- registers */

static void data_write(uint8_t value)
{
    if (address < 0x2000) {
        /* The patterns live in the cartridge. This program does not write
         * them: it is the cartridge's own memory and it arrives filled. */
    } else if (address < 0x3F00) {
        mmc5_nt_write(address, value);      /* the chip owns the nametables */
    } else {
        /* Everywhere else the program writes a number that stands for
         * something; here it writes the colour itself, so it is stored whole.
         * The real chip keeps six bits per colour, because its table of
         * colours is shorter than this stage's. */
        palette_ram[(address - 0x3F00) & 31] = value;
    }
    address++;
}

void ppu_write(uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0x2000:
        break;                  /* the two bits this stage would use, it does not */
    case 0x2001:
        mask = value;           /* which halves of the picture are drawn */
        break;
    case 0x2003:
        oam_addr = value;       /* where in the sprite list the next write goes */
        break;
    case 0x2004:
        oam[oam_addr++] = value;
        break;
    case 0x2005:
        /* Two writes make one position: across first, then down. */
        if (!scroll_write) { next_x = value; scroll_write = 1; }
        else               { next_y = value; scroll_write = 0; }
        break;
    case 0x2006:
        /* Two writes make one address: the high byte first. */
        if (!address_high) {
            address = (uint16_t)((uint16_t)value << 8);
            address_high = 1;
        } else {
            address = (uint16_t)((address & 0xFF00) | value);
            address_high = 0;
        }
        break;
    case 0x2007:
        data_write(value);
        break;
    default:
        break;
    }
}

uint8_t ppu_read(uint16_t addr)
{
    switch (addr) {
    case 0x2002: {
        /* Reading the status clears the flag, so a game cannot see the same
         * frame twice. The game's loop reads it once to throw away whatever
         * was there, then in a loop until a new frame arrives. */
        uint8_t value = (uint8_t)(vblank ? 0x80 : 0);
        vblank = 0;
        return value;
    }
    case 0x2004:
        return oam[oam_addr++];
    case 0x2007: {
        uint8_t value = 0;
        if (address >= 0x2000 && address < 0x3F00) value = mmc5_nt_read(address);
        else if (address >= 0x3F00) value = palette_ram[(address - 0x3F00) & 31];
        address++;
        return value;
    }
    default:
        return 0;
    }
}
