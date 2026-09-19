/*
 * Stage 2 — memory and pointers, taught on a picture that lives in RAM.
 *
 * Until now every pixel went straight to the panel the moment the program
 * computed it. That works for a test pattern and fails for anything else:
 * you cannot redraw what you did not keep, you cannot change one part of the
 * picture without redrawing all of it, and you cannot compare two pictures.
 *
 * So the picture moves into memory first. This stage is about the two ideas
 * that make that possible:
 *
 *   an address   memory is a long row of numbered bytes; an address is the
 *                number of one of them
 *   a pointer    a variable that holds an address instead of a value
 *
 * The picture itself is one byte per pixel, holding a colour number rather than
 * a colour, which is why a 320x240 screen fits in RAM at all: colours would
 * need two bytes each. Turning numbers into colours happens at the moment of
 * sending, in one small loop.
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

/* Eight colours, as in stage 01. Their order in this table is what the
 * framebuffer stores, so the table is the link between a byte in memory and a
 * colour on the panel. */
#define COLOUR_COUNT 8
static const uint16_t palette[COLOUR_COUNT] = {
    0x0000,   /* 0 black   */
    0xF800,   /* 1 red     */
    0x07E0,   /* 2 green   */
    0x001F,   /* 3 blue    */
    0xFFE0,   /* 4 yellow  */
    0xF81F,   /* 5 magenta */
    0x07FF,   /* 6 cyan    */
    0xFFFF,   /* 7 white   */
};

/* ---------------------------------------------------------------- memory
 * This is the picture, and it is nothing more than a long row of bytes: one
 * byte for every pixel on the screen, 320 across and 240 down, which is 76,800
 * bytes. Written out, the first byte belongs to the top-left pixel, the second
 * to the pixel to its right, and so on to the end of the row, after which the
 * next row begins.
 *
 * Because the bytes are in a known order, the byte for a pixel at column x and
 * row y sits at position (y * PANEL_W + x) in this row. That arithmetic is the
 * whole of two-dimensional drawing.
 */
static uint8_t framebuffer[PANEL_W * PANEL_H];

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

/* ------------------------------------------------------------------ pointers
 * An ampersand in front of a variable means "the address of this variable".
 * The result is a value like any other and can be stored in a variable of its
 * own: that is what a pointer is. The star in the type says what kind of thing
 * lives at that address, in this case a byte.
 *
 * Below, two functions do the same thing. The first one takes the address of
 * the buffer and walks it byte by byte; the second takes the whole array by
 * name, which the language quietly turns into its address anyway. Writing both
 * is the quickest way to see that an array name *is* an address.
 */
static void fill_with_pointer(uint8_t *pixels, int count, uint8_t value)
{
    /* The star in front of the pointer means "the byte at this address". */
    for (int i = 0; i < count; i++) {
        *pixels = value;      /* write through the pointer */
        pixels++;             /* move the pointer to the next byte */
    }
}

static void fill_with_array(uint8_t pixels[], int count, uint8_t value)
{
    for (int i = 0; i < count; i++) {
        pixels[i] = value;    /* exact same bytes, written the other way */
    }
}

/* Put one pixel into the picture. Nothing appears on the panel yet: this only
 * changes a byte in memory, which is the point of the whole stage. */
static void set_pixel(int x, int y, uint8_t colour)
{
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) {
        return;               /* outside the picture: do nothing */
    }
    framebuffer[y * PANEL_W + x] = colour;
}

static void draw_rectangle(int x0, int y0, int w, int h, uint8_t colour)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            set_pixel(x, y, colour);
        }
    }
}

/* The picture the reader will look at, drawn entirely in memory. */
static void draw_scene(void)
{
    fill_with_array(framebuffer, PANEL_W * PANEL_H, 3);      /* blue sky   */
    fill_with_pointer(framebuffer + (PANEL_H - 60) * PANEL_W,
                      60 * PANEL_W, 2);                      /* green grass */

    draw_rectangle(40, 60, 80, 80, 7);                       /* white box  */
    draw_rectangle(180, 100, 60, 120, 1);                    /* red tower  */
    draw_rectangle(200, 140, 20, 80, 4);                     /* yellow band */

    for (int i = 0; i < 40; i++) {                           /* a diagonal */
        set_pixel(280 + i / 2, 40 + i, 7);
    }
}

/* ------------------------------------------------- send the picture to the panel
 * The framebuffer holds numbers, the panel wants colours, so the bytes are
 * turned into colours here. Bands are not used yet: the whole picture goes out
 * in one run, and the panel waits for the end of it. Stage 04 explains why that
 * is a problem and how bands fix it.
 */
static void push_framebuffer(void)
{
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);

    dc(1);
    cs(0);
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        uint16_t colour = palette[framebuffer[i] & (COLOUR_COUNT - 1)];
        spi_byte((uint8_t)(colour >> 8));
        spi_byte((uint8_t)(colour & 0xFF));
    }
    cs(1);
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

    draw_scene();          /* changes bytes in memory, not one pixel on the panel */
    push_framebuffer();    /* and only now does the panel see anything */

    for (;;) { }
}
