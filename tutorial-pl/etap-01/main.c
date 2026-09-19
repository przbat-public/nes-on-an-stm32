/*
 * Stage 1 — a framebuffer, a palette, and bands.
 *
 * Stage 0 pushed colour straight at the panel: no memory in between, so nothing
 * could be redrawn, compared or timed. This stage adds the three ideas the whole
 * display path of the emulator rests on:
 *
 *   1. the clock goes from the power-on ~4 MHz to 80 MHz, so there is time to
 *      do work between pixels (the PLL arithmetic is spelled out in clock_init),
 *   2. the picture lives in a framebuffer in RAM as one byte per pixel, holding
 *      an index into a palette. One byte per pixel is what makes a 256x240
 *      picture cost 60 KB instead of 120,
 *   3. the picture leaves the chip in bands. A band is a strip of scanlines
 *      converted to RGB565 and handed to the SPI. Why bands and not the whole
 *      frame: because 122,880 bytes take 24.6 ms on the wire at 40 MHz, and the
 *      emulator will spend that time emulating the next band instead of waiting.
 *
 * The test pattern is deliberately asymmetric: colour bars plus a white marker in
 * one corner. A solid colour hides a 180-degree rotation, which is exactly the
 * mistake stage 0's exercise was meant to show you.
 */
#include <stdint.h>

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

#define PANEL_W   320
#define PANEL_H   240
#define NES_W     256            /* the emulated picture, centred in the panel */
#define NES_X     32
#define BAND_H    8              /* scanlines per SPI transfer                  */
#define BAND_BYTES (NES_W * BAND_H * 2)

/* The framebuffer holds palette indices, one byte per pixel, only for the NES
 * picture. The 32-pixel bars on either side of the panel are painted once at
 * boot and never touched again. */
static uint8_t fb[NES_W * PANEL_H];

/* A small palette for this stage: the emulator will carry all 64 NES colours,
 * this one carries eight so the table fits on the screen of your editor. */
static const uint16_t palette[8] = {
    0x0000,   /* black      */
    0xF800,   /* red        */
    0x07E0,   /* green      */
    0x001F,   /* blue       */
    0xFFE0,   /* yellow     */
    0xF81F,   /* magenta    */
    0x07FF,   /* cyan       */
    0xFFFF,   /* white      */
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

/* 16 MHz (HSI16) x 10 / 2 = 80 MHz. The three factors are the three fields of
 * the PLL register: the source, the multiplier N and the divider R. Running the
 * core at 80 MHz instead of 4 MHz is the difference between a display that
 * crawls and one that can be redrawn sixty times a second. */
static void clock_init(void)
{
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);  /* wait states + caches */

    RCC_CR |= (1u << 8);                       /* HSI16 on */
    while (!(RCC_CR & (1u << 10))) { }

    RCC_PLLCFGR = (2u << 0)                    /* source: HSI16            */
                | (10u << 8)                   /* N = 10  -> 160 MHz VCO   */
                | (1u << 24);                  /* R = 2   -> 80 MHz, enabled */
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

    /* BR = 0 means the clock is the peripheral clock divided by two: 40 MHz.
     * That is the fastest this part of the chip can drive SPI. */
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
    /* The datasheet asks for 120 ms after SLPOUT. The loop is a rough
     * calibration, so the number is generous on purpose: too short here and the
     * panel shows nothing at all. */
    cmd(CMD_SLPOUT); delay(4000000);
    cmd(CMD_DISPON); delay(800000);

    /* Paint the side bars black once: they are outside the NES picture. */
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    dc(1); cs(0);
    for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
        spi_byte(0); spi_byte(0);
    }
    cs(1);
}

/* Send one band of the framebuffer: convert the indices to RGB565 and push the
 * bytes out. The conversion is the only per-pixel work the display path does,
 * and it is why the framebuffer holds indices rather than colours. */
static void push_band(int y0)
{
    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    for (int y = 0; y < BAND_H; y++) {
        const uint8_t *row = &fb[(y0 + y) * NES_W];
        for (int x = 0; x < NES_W; x++) {
            uint16_t c = palette[row[x] & 7];
            spi_byte((uint8_t)(c >> 8));
            spi_byte((uint8_t)(c & 0xFF));
        }
    }
    cs(1);
}

/* ----------------------------------------------------------------- pattern */

/* Vertical colour bars, plus a white block in the top-left corner so that any
 * rotation or mirroring of the picture is obvious at a glance. */
static void draw_test_pattern(void)
{
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            fb[y * NES_W + x] = (uint8_t)((x / 32) & 7);
        }
    }
    for (int y = 8; y < 40; y++) {
        for (int x = 8; x < 40; x++) {
            fb[y * NES_W + x] = 7;             /* white marker */
        }
    }
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();

    draw_test_pattern();
    for (int y = 0; y < PANEL_H; y += BAND_H) {
        push_band(y);
    }

    for (;;) { }
}
