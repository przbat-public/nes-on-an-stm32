/*
 * Stage 0 — make the panel light up.
 *
 * This program does one thing: it turns the whole display one colour. There is
 * no emulator here yet, and no clock setup either: the chip runs on its power-on
 * clock (about 4 MHz), which is slow enough that a full screen takes a moment to
 * fill. Seeing that moment is the point of this stage. Stage 03 raises the clock
 * and explains what changed.
 *
 * What the panel needs before it will show anything, in order:
 *   1. a reset pulse on its RST pin,
 *   2. a short command sequence over SPI (colour format, orientation, wake up,
 *      display on),
 *   3. a window ("these are the pixels I am about to send") and then the pixels.
 *
 * The panel has no address bus and no way to read back what it holds. Every byte
 * we send goes straight into its frame memory, in the order we send it.
 */
#include <stdint.h>

/* ------------------------------------------------------------------ registers
 * A register is just a 32-bit word at a fixed address. These four addresses are
 * all this stage needs; docs/STYLE.md in the emulator repository says why the
 * firmware keeps them in one file (hal.c) rather than scattered through the
 * logic. Here they are at the top, where you can see them.
 */
#define REG32(addr)  (*(volatile uint32_t *)(addr))

#define RCC_AHB2ENR   REG32(0x40021000UL + 0x4C)  /* GPIO clock enable      */
#define RCC_APB2ENR   REG32(0x40021000UL + 0x60)  /* SPI1 clock enable      */
#define GPIOA_MODER   REG32(0x48000000UL + 0x00)  /* PA mode: in/out/AF     */
#define GPIOA_AFRL    REG32(0x48000000UL + 0x20)  /* PA alternate functions */
#define GPIOA_BSRR    REG32(0x48000000UL + 0x18)  /* PA set / reset         */
#define GPIOB_MODER   REG32(0x48000400UL + 0x00)
#define GPIOB_BSRR    REG32(0x48000400UL + 0x18)
#define SPI1_CR1      REG32(0x40013000UL + 0x00)
#define SPI1_CR2      REG32(0x40013000UL + 0x04)
#define SPI1_SR       REG32(0x40013000UL + 0x08)
#define SPI1_DR       REG32(0x40013000UL + 0x0C)

/* Which pin does what. The shield wires the panel to these, and only these. */
#define PIN_CS    9    /* PA9  chip select, active low: we drive it by hand */
#define PIN_DC    10   /* PB10 data/command: low = command, high = pixel    */
#define PIN_RST   1    /* PA1  reset                                         */
#define PIN_SCK   5    /* PA5  SPI1 clock     */
#define PIN_MOSI  7    /* PA7  SPI1 data out  */

/* ST7789 commands used here. The names come from the panel's datasheet. */
#define CMD_CASET   0x2A   /* column address window */
#define CMD_RASET   0x2B   /* row address window    */
#define CMD_RAMWR   0x2C   /* "the bytes that follow are pixels" */
#define CMD_MADCTL  0x36   /* memory access control: orientation and order */
#define CMD_COLMOD  0x3A   /* colour format */
#define CMD_SLPOUT  0x11   /* leave sleep mode */
#define CMD_DISPON  0x29   /* display on */

#define PANEL_W   320
#define PANEL_H   240

/* The colour: 16 bits per pixel, five bits red, six green, five blue (RGB565).
 * This one is a strong blue. */
#define COLOUR_BLUE  0x001Fu

/* ------------------------------------------------------------------- helpers */

static void delay(volatile uint32_t loops)
{
    while (loops--) { }
}

static void pin_set(int pin)
{
    GPIOA_BSRR = (1u << pin);
}

static void pin_clear(int pin)
{
    GPIOA_BSRR = (1u << (pin + 16));
}

static void cs(int on)  { if (on) pin_set(PIN_CS); else pin_clear(PIN_CS); }

static void dc(int data)
{
    if (data) GPIOB_BSRR = (1u << PIN_DC);
    else      GPIOB_BSRR = (1u << (PIN_DC + 16));
}

/* One byte out, waiting for the transmit register to be free first. This is the
 * whole SPI conversation: the panel is a shift register that watches two side
 * pins to know what the bytes mean. */
static void spi_byte(uint8_t b)
{
    while (!(SPI1_SR & (1u << 1))) { }     /* TXE: room in the transmit reg */
    SPI1_DR = b;
    while (!(SPI1_SR & (1u << 1))) { }
    (void)SPI1_SR;
    (void)SPI1_DR;
}

static void cmd(uint8_t c)
{
    dc(0);
    cs(0);
    spi_byte(c);
    cs(1);
}

static void data(uint8_t b)
{
    dc(1);
    cs(0);
    spi_byte(b);
    cs(1);
}

/* ---------------------------------------------------------------- bring-up */

static void gpio_init(void)
{
    RCC_AHB2ENR |= (1u << 0) | (1u << 1);      /* GPIOA and GPIOB clocks */

    /* PA5, PA7 as SPI1 alternate function 5; PA9 as a plain output for CS. */
    GPIOA_MODER &= ~((3u << (PIN_SCK * 2)) | (3u << (PIN_MOSI * 2))
                     | (3u << (PIN_CS * 2)));
    GPIOA_MODER |= (2u << (PIN_SCK * 2)) | (2u << (PIN_MOSI * 2))
                 | (1u << (PIN_CS * 2));
    GPIOA_AFRL &= ~((0xFu << (PIN_SCK * 4)) | (0xFu << (PIN_MOSI * 4)));
    GPIOA_AFRL |= (5u << (PIN_SCK * 4)) | (5u << (PIN_MOSI * 4));

    /* PB10 is the data/command pin. */
    GPIOB_MODER &= ~(3u << (PIN_DC * 2));
    GPIOB_MODER |= (1u << (PIN_DC * 2));

    /* PA1 resets the panel, and it must start high: the panel is held in reset
     * while the pin is low, and we pulse it in panel_init(). */
    GPIOA_MODER &= ~(3u << (PIN_RST * 2));
    GPIOA_MODER |= (1u << (PIN_RST * 2));
    pin_set(PIN_RST);
    cs(1);
}

static void spi_init(void)
{
    RCC_APB2ENR |= (1u << 12);                 /* SPI1 clock */

    /* Master, 8-bit, mode 0, software chip select, clock = peripheral clock / 2.
     * At the power-on clock that is about 2 MHz: slow, and deliberately so for
     * this stage. */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);                      /* 8-bit transfers */
    SPI1_CR1 |= (1u << 6);                     /* enable */
}

static void panel_init(void)
{
    pin_clear(PIN_RST);
    delay(200000);                             /* hold reset low */
    pin_set(PIN_RST);
    delay(1200000);                            /* the panel needs ~120 ms */

    cmd(CMD_COLMOD); data(0x55);               /* 16 bits per pixel, RGB565 */
    cmd(CMD_MADCTL); data(0x60);               /* landscape */
    cmd(CMD_SLPOUT); delay(1200000);
    cmd(CMD_DISPON); delay(200000);
}

/* Send one colour to every pixel of the panel. */
static void fill(uint16_t colour)
{
    cmd(CMD_CASET);
    data(0); data(0);
    data((uint8_t)((PANEL_W - 1) >> 8)); data((uint8_t)(PANEL_W - 1));

    cmd(CMD_RASET);
    data(0); data(0);
    data((uint8_t)((PANEL_H - 1) >> 8)); data((uint8_t)(PANEL_H - 1));

    cmd(CMD_RAMWR);
    dc(1);
    cs(0);
    for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
        spi_byte((uint8_t)(colour >> 8));      /* high byte first */
        spi_byte((uint8_t)(colour & 0xFF));
    }
    cs(1);
}

int main(void)
{
    gpio_init();
    spi_init();
    panel_init();
    fill(COLOUR_BLUE);

    for (;;) { }                               /* stay here and keep it lit */
}
