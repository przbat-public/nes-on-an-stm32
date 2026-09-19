/*
 * Stage 8 — how the console builds a picture: patterns, a map, and palettes.
 *
 * Stage 4 filled a framebuffer by hand, byte after byte. That is how you draw a
 * test pattern, and it is not how a game is drawn. A screen holds 61,440
 * pixels: far more than a cartridge of that era could store, and far more than
 * the picture chip could fetch one at a time.
 *
 * So the console never keeps a picture. It keeps three much smaller things:
 *
 *   1. a set of 8x8 patterns, 16 bytes each, living in the cartridge,
 *   2. a map of which pattern goes where: 32 by 30 numbers, 960 bytes for a
 *      whole screen,
 *   3. four palettes of four colours, 16 bytes in all, plus one byte of four
 *      palette numbers per square of 32 by 32 pixels.
 *
 * All three sit in cartridge.h, taken from one cartridge. The work of this
 * stage is to turn them into the framebuffer of stage 4, pattern by pattern.
 * Nothing moves yet: this is one frozen frame, read from the cartridge before
 * the game had a chance to change it.
 *
 * The panel code below is stage 4 unchanged — same clock, same bands, same
 * black bars beside the picture. Read stage 4's chapter if any of it is new.
 */
#include <stdint.h>

#include "cartridge.h"

#define REG32(addr)  (*(volatile uint32_t *)(addr))

#define FLASH_ACR     REG32(0x40022000UL + 0x00)
#define RCC_CR        REG32(0x40021000UL + 0x00)
#define RCC_PLLCFGR   REG32(0x40021000UL + 0x0C)
#define RCC_CFGR      REG32(0x40021000UL + 0x08)
#define RCC_AHB2ENR   REG32(0x40021000UL + 0x4C)
#define RCC_APB2ENR   REG32(0x40021000UL + 0x60)
#define GPIOA_MODER   REG32(0x48000000UL + 0x00)
#define GPIOA_AFRL    REG32(0x48000000UL + 0x20)
#define GPIOA_BSRR    REG32(0x48000000UL + 0x18)
#define GPIOB_MODER   REG32(0x48000400UL + 0x00)
#define GPIOB_BSRR    REG32(0x48000400UL + 0x18)
#define SPI1_CR1      REG32(0x40013000UL + 0x00)
#define SPI1_CR2      REG32(0x40013000UL + 0x04)
#define SPI1_SR       REG32(0x40013000UL + 0x08)
#define SPI1_DR       REG32(0x40013000UL + 0x0C)

#define PIN_CS    9
#define PIN_DC    10
#define PIN_RST   1
#define PIN_SCK   5
#define PIN_MOSI  7

#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_SLPOUT  0x11
#define CMD_DISPON  0x29

/* The picture the console sends out is 256 by 240 pixels, which is 32 tiles
 * across and 30 tiles down. The panel is 320 pixels wide, so the picture sits
 * in the middle with a black bar on either side. */
#define NES_W     256
#define NES_H     240
#define NES_X     32
#define TILE_PX   8
#define TILES_X   32             /* must match the nametable in cartridge.h */
#define TILES_Y   30

#define PANEL_W   320
#define PANEL_H   240
#define BAND_H    8              /* scanlines per SPI transfer */

/* The framebuffer from stage 4: one byte per pixel, holding a colour number
 * rather than a colour. The numbers are now the console's own, 0 to 63. */
static uint8_t fb[NES_W * NES_H];

/* The sixty-four colours the console can name, written the way the panel wants
 * them. The console has no such table inside: it puts a number on a wire and
 * lets the television decide what that number looks like, so every table like
 * this one is an agreed approximation. This is the emulator's. */
static const uint16_t colour_rgb565[64] = {
    0x52AA, 0x00EE, 0x0892, 0x3011, 0x400C, 0x5806, 0x5020, 0x38C0,   /* 0x00-0x07 */
    0x2140, 0x09C0, 0x0200, 0x01E0, 0x0187, 0x0000, 0x0000, 0x0000,   /* 0x08-0x0F */
    0x9CB3, 0x0A78, 0x319D, 0x58FC, 0x88B6, 0xA0AC, 0x9904, 0x79E0,   /* 0x10-0x17 */
    0x52C0, 0x2B80, 0x0BE0, 0x03A5, 0x032F, 0x0000, 0x0000, 0x0000,   /* 0x18-0x1F */
    0xEF7D, 0x4CDD, 0x7BFD, 0xB31D, 0xE2BD, 0xEAD6, 0xEB4C, 0xD444,   /* 0x20-0x27 */
    0xA540, 0x7620, 0x4E84, 0x3E6D, 0x3DB9, 0x39E7, 0x0000, 0x0000,   /* 0x28-0x2F */
    0xEF7D, 0xAE7D, 0xBDFD, 0xD59D, 0xED7D, 0xED7A, 0xEDB6, 0xE632,   /* 0x30-0x37 */
    0xCE8F, 0xB6EF, 0xAF12, 0x9F16, 0xA6BC, 0xA514, 0x0000, 0x0000,   /* 0x38-0x3F */
};

/* ------------------------------------------------------------------ helpers */

static void delay(volatile uint32_t loops) { while (loops--) { } }
static void pin_set(int p)   { GPIOA_BSRR = (1u << p); }
static void pin_clear(int p) { GPIOA_BSRR = (1u << (p + 16)); }
static void cs(int on)       { if (on) pin_set(PIN_CS); else pin_clear(PIN_CS); }
static void dc(int data)
{
    if (data) GPIOB_BSRR = (1u << PIN_DC);
    else      GPIOB_BSRR = (1u << (PIN_DC + 16));
}

static void spi_byte(uint8_t b)
{
    while (!(SPI1_SR & (1u << 1))) { }
    SPI1_DR = b;
    while (!(SPI1_SR & (1u << 1))) { }
    (void)SPI1_SR;
    (void)SPI1_DR;
}

static void cmd(uint8_t c)  { dc(0); cs(0); spi_byte(c); cs(1); }
static void data(uint8_t b) { dc(1); cs(0); spi_byte(b); cs(1); }

/* ------------------------------------------------------------------- clocks */

/* 16 MHz (HSI16) x 10 / 2 = 80 MHz, spelled out in stage 4: the source, the
 * multiplier and the divider are three fields of one register. */
static void clock_init(void)
{
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);  /* wait states + caches */

    RCC_CR |= (1u << 8);                       /* HSI16 on */
    while (!(RCC_CR & (1u << 10))) { }

    RCC_PLLCFGR = (2u << 0)                    /* source: HSI16              */
                | (10u << 8)                   /* N = 10 -> 160 MHz VCO      */
                | (1u << 24);                  /* R = 2  -> 80 MHz, enabled  */
    RCC_CR |= (1u << 24);
    while (!(RCC_CR & (1u << 25))) { }

    RCC_CFGR = (3u << 0);                      /* switch the core to the PLL */
    while ((RCC_CFGR & (3u << 2)) != (3u << 2)) { }
}

static void gpio_init(void)
{
    RCC_AHB2ENR |= (1u << 0) | (1u << 1);

    GPIOA_MODER &= ~((3u << (PIN_SCK * 2)) | (3u << (PIN_MOSI * 2))
                     | (3u << (PIN_CS * 2)) | (3u << (PIN_RST * 2)));
    GPIOA_MODER |= (2u << (PIN_SCK * 2)) | (2u << (PIN_MOSI * 2))
                 | (1u << (PIN_CS * 2)) | (1u << (PIN_RST * 2));
    GPIOA_AFRL &= ~((0xFu << (PIN_SCK * 4)) | (0xFu << (PIN_MOSI * 4)));
    GPIOA_AFRL |= (5u << (PIN_SCK * 4)) | (5u << (PIN_MOSI * 4));

    GPIOB_MODER &= ~(3u << (PIN_DC * 2));
    GPIOB_MODER |= (1u << (PIN_DC * 2));

    pin_set(PIN_RST);
    cs(1);
}

static void spi_init(void)
{
    RCC_APB2ENR |= (1u << 12);

    /* BR = 0 means the peripheral clock divided by two: 40 MHz. */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);
}

/* -------------------------------------------------------------------- panel */

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    cmd(CMD_CASET);
    data((uint8_t)(x0 >> 8)); data((uint8_t)x0);
    data((uint8_t)(x1 >> 8)); data((uint8_t)x1);
    cmd(CMD_RASET);
    data((uint8_t)(y0 >> 8)); data((uint8_t)y0);
    data((uint8_t)(y1 >> 8)); data((uint8_t)y1);
    cmd(CMD_RAMWR);
}

static void panel_init(void)
{
    pin_clear(PIN_RST); delay(400000);         /* 400k loops at 80 MHz ~ 20 ms */
    pin_set(PIN_RST);   delay(2000000);        /* ~120 ms */

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(4000000);
    cmd(CMD_DISPON); delay(800000);

    /* The bars beside the console's picture are not part of it, so they are
     * painted black once, here, and never touched again. */
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    dc(1); cs(0);
    for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
        spi_byte(0); spi_byte(0);
    }
    cs(1);
}

/* Send one band of eight scanlines. The framebuffer holds console colour
 * numbers and the panel wants RGB565, so every byte goes through the table on
 * its way out; that lookup is the only per-pixel work on this path. */
static void push_band(int y0)
{
    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    for (int y = 0; y < BAND_H; y++) {
        const uint8_t *row = &fb[(y0 + y) * NES_W];
        for (int x = 0; x < NES_W; x++) {
            /* A palette byte uses six bits; masking the top two off keeps the
             * index inside the table. */
            uint16_t colour = colour_rgb565[row[x] & 0x3F];
            spi_byte((uint8_t)(colour >> 8));
            spi_byte((uint8_t)(colour & 0xFF));
        }
    }
    cs(1);
}

/* ---------------------------------------------------------------- the picture
 * Everything below turns the tables of cartridge.h into the framebuffer. Not
 * one pixel of a pattern is drawn here cleverly: patterns are copied, 8 by 8 at
 * a time, and the colour of each of their pixels comes from a palette.
 */

/* A pixel of a pattern is a number from 0 to 3. The two halves of a tile hold
 * one bit each, taken from the same position in both, so the value is the low
 * bit plus twice the high bit. Bits are counted from the left, the way the
 * pixels are: column 0 is bit 7. */
static int pixel_value(const uint8_t *pattern, int row, int col)
{
    int bit  = 7 - col;
    int low  = (pattern[row] >> bit) & 1;
    int high = (pattern[row + TILE_PX] >> bit) & 1;
    return low | (high << 1);
}

/* Which of the four palettes this tile uses. The attribute table spends one
 * byte on every square of 4 by 4 tiles, and that byte holds four palette
 * numbers, one for each quarter of the square. So the colour changes on the
 * border of a 16-by-16 square and never in the middle of a tile. */
static int block_palette(int tx, int ty)
{
    uint8_t pair = attributes[(ty / 4) * (TILES_X / 4) + (tx / 4)];
    int quarter = ((ty & 2) ? 4 : 0) + ((tx & 2) ? 2 : 0);
    return (pair >> quarter) & 3;
}

/* Copy one pattern into the framebuffer, painting its numbers with the colours
 * of one palette. */
static void draw_tile(int tx, int ty, int tile, int palette_number)
{
    const uint8_t *pattern = &chr_rom[tile * TILE_BYTES];
    const uint8_t *colours = &bg_palette[palette_number * 4];
    int left = tx * TILE_PX;
    int top  = ty * TILE_PX;

    for (int row = 0; row < TILE_PX; row++) {
        for (int col = 0; col < TILE_PX; col++) {
            int value = pixel_value(pattern, row, col);

            /* Value 0 does not mean the first colour of this palette. It means
             * "no colour", and the console paints the one background colour of
             * the screen there instead. That is why all four palettes in
             * cartridge.h begin with the same byte. */
            uint8_t colour = (value == 0) ? bg_palette[0] : colours[value];

            fb[(top + row) * NES_W + left + col] = colour;
        }
    }
}

/* Walk the map: for each of the 32 by 30 positions take the pattern number from
 * the nametable and paint it with the palette its square asks for. */
static void draw_background(void)
{
    for (int ty = 0; ty < TILES_Y; ty++) {
        for (int tx = 0; tx < TILES_X; tx++) {
            int tile = nametable[ty * TILES_X + tx];

            /* This cartridge leaves patterns 50 and up empty and the map never
             * asks for them, so they are not in the table at all. */
            if (tile >= TILE_COUNT) {
                continue;
            }
            draw_tile(tx, ty, tile, block_palette(tx, ty));
        }
    }
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();

    /* The picture is built in memory first and sent afterwards. That order is
     * the point of stage 2: while draw_background runs, the panel is still
     * showing the previous frame, which this time is the black it was painted
     * with during setup. */
    draw_background();

    for (int y = 0; y < NES_H; y += BAND_H) {
        push_band(y);
    }

    for (;;) { }
}
