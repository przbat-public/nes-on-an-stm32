/*
 * Stage 1 — the language, taught on colours.
 *
 * Stage 00 was one long list of instructions. This stage puts a picture on the
 * panel again, but now the program is built from the ideas every C program is
 * made of, and each of them changes something you can see:
 *
 *   a constant   colours and the panel size get names, so the code says what it
 *                means instead of repeating bare numbers
 *   a variable   how many bands to draw is a number in one place
 *   an array     the colours sit in one table, reached by an index
 *   a function   "fill a rectangle with this colour" is written once and reused
 *   a loop       the bands are drawn by repeating that function
 *   a condition  the last band is white whatever the palette says
 *
 * The panel bring-up at the bottom is stage 00 unchanged: read the top half of
 * this file with the chapter and treat the bottom half as machinery you already
 * got working once.
 */
#include <stdint.h>

#define REG32(addr)  (*(volatile uint32_t *)(addr))
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

/* ---------------------------------------------------------------- constants
 * A constant is a number with a name. Written with #define, the name is
 * replaced before compilation, so it costs nothing at run time. Two reasons to
 * use one: the code reads like a description, and a value lives in one place.
 */
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

#define PANEL_W   320
#define PANEL_H   240

/* Colours, written the way the panel wants them: five bits of red, six of
 * green, five of blue, packed into sixteen bits. */
#define COLOUR_BLACK    0x0000
#define COLOUR_RED      0xF800
#define COLOUR_GREEN    0x07E0
#define COLOUR_BLUE     0x001F
#define COLOUR_YELLOW   0xFFE0
#define COLOUR_MAGENTA  0xF81F
#define COLOUR_CYAN     0x07FF
#define COLOUR_WHITE    0xFFFF

/* A variable: how many bands to draw. Change this one number and the picture
 * changes, which is what a variable is for. */
static int bands = 8;

/* An array: many values of the same kind, reached by a number called the index.
 * palette[0] is black, palette[1] is red, and so on. */
static const uint16_t palette[] = {
    COLOUR_BLACK, COLOUR_RED,     COLOUR_GREEN, COLOUR_BLUE,
    COLOUR_YELLOW, COLOUR_MAGENTA, COLOUR_CYAN,  COLOUR_WHITE,
};
#define PALETTE_SIZE (sizeof(palette) / sizeof(palette[0]))

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

static void set_window(int x0, int y0, int x1, int y1)
{
    cmd(CMD_CASET);
    data((uint8_t)(x0 >> 8)); data((uint8_t)x0);
    data((uint8_t)(x1 >> 8)); data((uint8_t)x1);
    cmd(CMD_RASET);
    data((uint8_t)(y0 >> 8)); data((uint8_t)y0);
    data((uint8_t)(y1 >> 8)); data((uint8_t)y1);
    cmd(CMD_RAMWR);
}

/* ------------------------------------------------------------------ functions
 * A function is a piece of work with a name. It takes values in through its
 * parameters (each with a type, in the brackets) and hands one value back
 * through its return type; void means it hands nothing back.
 *
 * This one fills a rectangle. Everything the program draws goes through it, so
 * the loop over pixels is written once in the whole file.
 */
static void fill_rectangle(int x0, int y0, int w, int h, uint16_t colour)
{
    set_window(x0, y0, x0 + w - 1, y0 + h - 1);

    dc(1);
    cs(0);
    for (int y = 0; y < h; y++) {               /* a loop inside a function */
        for (int x = 0; x < w; x++) {
            spi_byte((uint8_t)(colour >> 8));   /* high byte first */
            spi_byte((uint8_t)(colour & 0xFF));
        }
    }
    cs(1);
}

/* The picture: horizontal bands, one colour each, taken from the array. */
static void draw_bands(void)
{
    int band_height = PANEL_H / bands;   /* integer division: 240 / 8 = 30 */

    for (int i = 0; i < bands; i++) {    /* i runs 0, 1, 2, ... bands - 1 */
        int y = i * band_height;
        uint16_t colour = palette[i % PALETTE_SIZE];

        /* A condition: the last band is white whatever the palette says, so the
         * bottom edge of the picture is always recognisable. */
        if (i == bands - 1) {
            colour = COLOUR_WHITE;
        }

        fill_rectangle(0, y, PANEL_W, band_height, colour);
    }
}

/* -------------------------------------------------------------------- panel */

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
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);
}

static void panel_init(void)
{
    pin_clear(PIN_RST); delay(200000);
    pin_set(PIN_RST);   delay(1200000);

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(1200000);
    cmd(CMD_DISPON); delay(200000);
}

int main(void)
{
    gpio_init();
    spi_init();
    panel_init();

    draw_bands();        /* one call, and the whole picture is on the panel */

    for (;;) { }
}
