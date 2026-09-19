/*
 * ppu.c — video memory, and the walk that turns it into pixels.
 *
 * The processor never sees a pixel. It writes tile numbers and palette
 * numbers into ranges of memory that are not memory at all: the addresses
 * $2000-$2007 are six registers of this chip, and a write there changes what
 * the picture will look like next time it is drawn.
 *
 * Which is the same idea as the cartridge's bank register, one level up.
 * There, an address in the program's range meant "some bank or other"; here,
 * an address in the processor's range means "the picture chip". A bus that
 * decides who answers is what a memory map is.
 */
#include "ppu.h"

#define VRAM_SIZE   (30 * 32)     /* 30 rows of 32 tiles */
#define CHR_SIZE    64            /* four tiles, 16 bytes each */
#define PALETTE_SIZE 32          /* 16 colours, stored twice over         */
#define COLOUR_COUNT 16          /* how many colours the chip can show    */

static uint8_t vram[VRAM_SIZE];      /* which tile is where            */
static uint8_t chr[CHR_SIZE];        /* the four tile shapes           */
static uint8_t palette[PALETTE_SIZE];

static uint16_t address;             /* where the next write goes      */
static bool     address_high;        /* the two-write latch            */
static uint8_t  scroll_x, scroll_y;  /* where the picture is scrolled  */
static uint8_t  control, mask, status;

void ppu_reset(void)
{
    for (int i = 0; i < VRAM_SIZE; i++)    vram[i] = 0;
    for (int i = 0; i < CHR_SIZE; i++)     chr[i] = 0;
    for (int i = 0; i < PALETTE_SIZE; i++) palette[i] = 0;
    address = 0;
    address_high = false;
    scroll_x = scroll_y = 0;
    control = mask = status = 0;
}

void ppu_start_frame(void)
{
    status |= 0x80;                  /* bit 7: a new frame has begun */
}

uint8_t ppu_read(uint16_t addr)
{
    if (addr == 0x2002) {
        /* Reading the status register also clears the flag, so the game
         * cannot see the same frame twice. The game reads it twice: once
         * to throw away an old flag, then in a loop until a new one
         * arrives. */
        uint8_t value = status;
        status &= (uint8_t)~0x80;
        return value;
    }

    if (addr == 0x2007) {           /* read the byte the address points at */
        uint16_t at = address;
        uint8_t  value;

        if (at < 0x2000)          value = chr[at & (CHR_SIZE - 1)];
        else if (at < 0x3F00)     value = vram[(at - 0x2000) % VRAM_SIZE];
        else                      value = palette[(at - 0x3F00) & (PALETTE_SIZE - 1)];

        address = (uint16_t)((address + 1) & 0x3FFF);
        return value;
    }

    return 0;
}

void ppu_write(uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0x2000:
        /* Only bit 3 of the mask is looked at by this stage's renderer, so
         * the control register is stored and otherwise ignored. */
        control = value;
        (void)control;
        break;

    case 0x2001:
        mask = value;
        break;

    case 0x2005:
        /* Two writes: the first is how far right the picture is scrolled,
         * the second how far down. Both are counted in pixels. */
        if (!address_high) scroll_x = value;
        else               scroll_y = value;
        address_high = !address_high;
        break;

    case 0x2006:
        /* Two writes make one 14-bit address, high byte first: the same
         * shape as the cartridge's bank register, with more bits. */
        if (!address_high) address = (uint16_t)((address & 0x00FF) | (value << 8));
        else               address = (uint16_t)((address & 0xFF00) | value);
        address = (uint16_t)(address & 0x3FFF);
        address_high = !address_high;
        break;

    case 0x2007:
        if (address < 0x2000) {
            chr[address & (CHR_SIZE - 1)] = value;
        } else if (address < 0x3F00) {
            vram[(address - 0x2000) % VRAM_SIZE] = value;
        } else {
            palette[(address - 0x3F00) & (PALETTE_SIZE - 1)] = value & 0x3F;
        }
        /* The address moves on by itself after every byte, so filling the
         * screen is one `sta` inside a loop and nothing else. */
        address = (uint16_t)((address + 1) & 0x3FFF);
        break;

    default:
        break;
    }
}



void ppu_render_picture(uint8_t *pixels)
{
    /* With the background switched off the screen shows one colour: the
     * one in the palette's first entry. */
    if (!(mask & 0x08)) {
        for (int i = 0; i < PPU_W * PPU_H; i++)
            pixels[i] = palette[0];
        return;
    }

    for (int y = 0; y < PPU_H; y++) {
        /* Scrolling does not move anything in memory: it moves where the
         * walk starts, and wraps around the edges. */
        int ty = (y + scroll_y) % (30 * 8);
        for (int x = 0; x < PPU_W; x++) {
            int tx = (x + scroll_x) % (32 * 8);

            uint8_t tile  = vram[(ty / 8) * 32 + (tx / 8)];
            uint8_t shape = chr[tile * 16 + (ty % 8)];

            /* One byte of a tile shape holds eight pixels, one bit each,
             * most significant bit on the left. A set bit means "pixel
             * value 1", and pixel value 0 means "show the background". */
            unsigned bit = 7u - (unsigned)(tx % 8);
            uint8_t value = (uint8_t)((shape >> bit) & 1u);

            /* The palette byte is a colour *number*, never an index into
             * the palette array: those are two different things, and
             * confusing them shows up as a picture in the wrong colours. */
            pixels[y * PPU_W + x] = palette[value & (COLOUR_COUNT - 1)];
        }
    }
}
