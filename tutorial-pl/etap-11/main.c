/*
 * Stage 11 — control: how the console reads its pad over one wire.
 *
 * Until now the picture did only what the program decided. This stage hands
 * the program something the player controls, and it does it the way the
 * console does: not as five separate wires, but as one wire that carries
 * eight bits, one after another, out of a shift register inside the pad.
 *
 * Three ideas, in the order they appear below:
 *
 *   1. a switch is a break in a wire. Pressed, it connects its pin to ground,
 *      and a resistor inside the chip supplies the other level, so five
 *      switches turn into one byte (read_pad_pins),
 *   2. between the switches and the console sits a shift register: a row of
 *      eight cells that copies every switch at once and then hands them over
 *      one bit per pulse on the clock wire (pad_write_latch, pad_read_bit),
 *   3. the program asks for that single wire eight times at the start of a
 *      frame and puts the bits back into a byte — the byte that eight wires
 *      would have given it (read_pad).
 *
 * The emulated program in this stage is a handful of lines: it reads the byte
 * once per frame and moves the hero with it. A cartridge program does exactly
 * the same thing through the same protocol, and that is the point: the game
 * knows nothing about our five switches, so the code below plays the part of
 * the pad and hands it a byte.
 *
 * The picture is a framebuffer of palette indices, as in the earlier stages.
 * New on the screen: the hero moves when the stick does, and a row of eight
 * small squares shows the byte as the emulated program received it.
 */
#include <stdint.h>

#define REG32(addr)  (*(volatile uint32_t *)(addr))

/* The GPIO ports sit 0x400 bytes apart, so one base address and one offset
 * describe all of them: at offset 0x10, for instance, each port has its own
 * input register. */
#define PORT_A   0x48000000UL
#define PORT_B   0x48000400UL
#define PORT_C   0x48000800UL

#define GPIO_MODER(base) REG32((base) + 0x00)
#define GPIO_PUPDR(base) REG32((base) + 0x0C)
#define GPIO_IDR(base)   REG32((base) + 0x10)
#define GPIO_BSRR(base)  REG32((base) + 0x18)
#define GPIO_AFRL(base)  REG32((base) + 0x20)

#define FLASH_ACR     REG32(0x40022000UL + 0x00)
#define RCC_CR        REG32(0x40021000UL + 0x00)
#define RCC_PLLCFGR   REG32(0x40021000UL + 0x0C)
#define RCC_CFGR      REG32(0x40021000UL + 0x08)
#define RCC_AHB2ENR   REG32(0x40021000UL + 0x4C)
#define RCC_APB2ENR   REG32(0x40021000UL + 0x60)
#define SPI1_CR1      REG32(0x40013000UL + 0x00)
#define SPI1_CR2      REG32(0x40013000UL + 0x04)
#define SPI1_SR       REG32(0x40013000UL + 0x08)
#define SPI1_DR       REG32(0x40013000UL + 0x0C)

/* The block where the chip is told which debug interface may own its pins. */
#define SYSCFG_DEBUG  REG32(0x40010000UL)

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

#define PANEL_W    320
#define PANEL_H    240
#define PICTURE_W  256          /* the console's picture, centred on the panel */
#define PICTURE_X  32

/* A byte in the framebuffer is a number, and the number is a place in this
 * table. Eight colours are enough for a scene with one hero in it. */
#define COL_BLACK   0
#define COL_RED     1
#define COL_GREEN   2
#define COL_BLUE    3
#define COL_YELLOW  4
#define COL_MAGENTA 5
#define COL_CYAN    6
#define COL_WHITE   7

static const uint16_t palette[8] = {
    0x0000,   /* 0 black   */
    0xF800,   /* 1 red     */
    0x07E0,   /* 2 green   */
    0x001F,   /* 3 blue    */
    0xFFE0,   /* 4 yellow  */
    0xF81F,   /* 5 magenta */
    0x07FF,   /* 6 cyan    */
    0xFFFF,   /* 7 white   */
};

static uint8_t fb[PICTURE_W * PANEL_H];

/* The eight bits the pad shifts out, in the order they come out: A first,
 * right last. That order is built into the pad's wiring, so a program that
 * reads the bits in a different order gets the buttons mixed up. */
#define PAD_A      0x01
#define PAD_B      0x02
#define PAD_SELECT 0x04
#define PAD_START  0x08
#define PAD_UP     0x10
#define PAD_DOWN   0x20
#define PAD_LEFT   0x40
#define PAD_RIGHT  0x80

/* ------------------------------------------------------------------ helpers */

static void delay(volatile uint32_t loops) { while (loops--) { } }

static void pin_set(int pin)   { GPIO_BSRR(PORT_A) = (1u << pin); }
static void pin_clear(int pin) { GPIO_BSRR(PORT_A) = (1u << (pin + 16)); }

static void cs(int on)
{
    if (on) GPIO_BSRR(PORT_A) = (1u << PIN_CS);
    else    GPIO_BSRR(PORT_A) = (1u << (PIN_CS + 16));
}

static void dc(int data)
{
    if (data) GPIO_BSRR(PORT_B) = (1u << PIN_DC);
    else      GPIO_BSRR(PORT_B) = (1u << (PIN_DC + 16));
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

/* The picture lives in memory, so one rectangle is one small loop over the
 * bytes that belong to it. */
static void fill_rectangle(int x0, int y0, int w, int h, uint8_t colour)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && x < PICTURE_W && y >= 0 && y < PANEL_H)
                fb[y * PICTURE_W + x] = colour;
        }
    }
}

/* ------------------------------------------------------------------- clocks */

/* 16 MHz (HSI16) x 10 / 2 = 80 MHz: source, multiplier N and divider R, the
 * three fields of one register. */
static void clock_init(void)
{
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);

    RCC_CR |= (1u << 8);
    while (!(RCC_CR & (1u << 10))) { }

    RCC_PLLCFGR = (2u << 0) | (10u << 8) | (1u << 24);
    RCC_CR |= (1u << 24);
    while (!(RCC_CR & (1u << 25))) { }

    RCC_CFGR = (3u << 0);
    while ((RCC_CFGR & (3u << 2)) != (3u << 2)) { }
}

static void gpio_init(void)
{
    RCC_AHB2ENR |= (1u << 0) | (1u << 1) | (1u << 2);   /* ports A, B, C */

    GPIO_MODER(PORT_A) &= ~((3u << (PIN_SCK * 2)) | (3u << (PIN_MOSI * 2))
                            | (3u << (PIN_CS * 2)) | (3u << (PIN_RST * 2)));
    GPIO_MODER(PORT_A) |= (2u << (PIN_SCK * 2)) | (2u << (PIN_MOSI * 2))
                        | (1u << (PIN_CS * 2)) | (1u << (PIN_RST * 2));
    GPIO_AFRL(PORT_A) &= ~((0xFu << (PIN_SCK * 4)) | (0xFu << (PIN_MOSI * 4)));
    GPIO_AFRL(PORT_A) |= (5u << (PIN_SCK * 4)) | (5u << (PIN_MOSI * 4));

    GPIO_MODER(PORT_B) &= ~(3u << (PIN_DC * 2));
    GPIO_MODER(PORT_B) |= (1u << (PIN_DC * 2));

    pin_set(PIN_RST);
    cs(1);
}

static void spi_init(void)
{
    RCC_APB2ENR |= (1u << 12);

    /* BR = 0 divides the peripheral clock by two: 40 MHz on the wire. */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);
}

static void panel_init(void)
{
    pin_clear(PIN_RST); delay(400000);
    pin_set(PIN_RST);   delay(2000000);

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(4000000);
    cmd(CMD_DISPON); delay(800000);

    /* Paint the whole panel black once: the picture is narrower than the
     * panel, so the side bars only ever need this one visit. */
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    dc(1); cs(0);
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        spi_byte(0); spi_byte(0);
    }
    cs(1);
}

/* The framebuffer holds palette numbers, the panel wants colours. */
static void push_picture(void)
{
    set_window(PICTURE_X, 0, PICTURE_X + PICTURE_W - 1, PANEL_H - 1);

    dc(1); cs(0);
    for (int i = 0; i < PICTURE_W * PANEL_H; i++) {
        uint16_t colour = palette[fb[i] & 7];
        spi_byte((uint8_t)(colour >> 8));
        spi_byte((uint8_t)(colour & 0xFF));
    }
    cs(1);
}

/* ------------------------------------------------------------------ the pad
 * Four functions, in the order the signal travels: pins, shift register, and
 * the byte the emulated program gets to use.
 */

/* Which contact of the stick is soldered to which pin, and which bit of the
 * pad byte it carries. The contacts are fixed to the board, but the picture is
 * drawn in landscape, so the board is held a quarter turn from the upright
 * hold in which the panel is taller than wide. The stick turns with the board,
 * which is why the edges in the comments below do not match the directions:
 * the contact on the board's right edge is the one you push to go up. */
typedef struct { uint32_t port; uint8_t pin; uint8_t bit; } pad_pin;

static const pad_pin pad_map[] = {
    { PORT_B,  0, PAD_UP    },   /* board's right edge: up after the turn */
    { PORT_B,  4, PAD_RIGHT },   /* board's bottom edge                   */
    { PORT_B,  6, PAD_DOWN  },   /* board's left edge                     */
    { PORT_C,  0, PAD_LEFT  },   /* board's top edge                      */
    { PORT_C, 13, PAD_A     },   /* the blue button, on the board itself  */
};

#define PAD_SWITCHES ((int)(sizeof(pad_map) / sizeof(pad_map[0])))

static void input_init(void)
{
    /* PB4 belongs to the chip's debug port at reset: that is where the pin
     * called NJTRST lives. While the debug port owns it, the contact soldered
     * to it cannot be read — the pin answers the debug hardware instead of the
     * input register, so one direction looks pressed all the time. The setup
     * below takes the pin back, and this register says that debugging stays on
     * SWD, the two-pin interface (PA13 and PA14) a debugger really uses; those
     * pins are never touched, so you can still attach one later. */
    RCC_APB2ENR |= (1u << 0);                     /* clock for that block */
    SYSCFG_DEBUG = (SYSCFG_DEBUG & ~(7u << 24)) | (2u << 24);

    for (int i = 0; i < PAD_SWITCHES; i++) {
        uint32_t port = pad_map[i].port;
        int      pin  = pad_map[i].pin;

        /* An unpressed switch connects the pin to nothing, so the pin would
         * pick up whatever noise is around. A resistor inside the chip holds
         * it high instead, which is why an unpressed switch reads 1. */
        GPIO_MODER(port) &= ~(3u << (pin * 2));                  /* input */
        GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << (pin * 2)))
                         | (1u << (pin * 2));                    /* pull-up */
    }
}

/* Five pins, one byte: every switch that is pressed sets its bit. */
static uint8_t read_pad_pins(void)
{
    uint8_t pad = 0;

    for (int i = 0; i < PAD_SWITCHES; i++) {
        if (!(GPIO_IDR(pad_map[i].port) & (1u << pad_map[i].pin)))
            pad = (uint8_t)(pad | pad_map[i].bit);
    }
    return pad;
}

/* The row of eight cells. The latch wire says "copy the switches now"; while
 * it is high the switches are copied straight through, and when it drops the
 * row is frozen and ready to be shifted out. */
static uint8_t shift_register;
static int     latch;

static void pad_write_latch(int level)
{
    latch = level;
    if (level)
        shift_register = read_pad_pins();
}

/* One pulse on the clock wire: the bottom cell goes out, every other cell
 * moves one place down, and the top cell fills with 1 — a wire that nobody
 * drives sits high. */
static int pad_read_bit(void)
{
    if (latch)
        return read_pad_pins() & PAD_A;

    int bit = shift_register & 1;
    shift_register = (uint8_t)((shift_register >> 1) | 0x80);
    return bit;
}

/* What a game does at the start of every frame: latch, then eight reads, each
 * bit landing in its own place in the byte. */
static uint8_t read_pad(void)
{
    pad_write_latch(1);
    pad_write_latch(0);

    uint8_t pad = 0;
    for (int i = 0; i < 8; i++)
        pad = (uint8_t)(pad | (pad_read_bit() << i));
    return pad;
}

/* -------------------------------------------------- the emulated program
 * A cartridge program reads the pad the same way: it gets a byte once per
 * frame and moves things with it. Ours moves the hero, and the blue button
 * marks him.
 */
/* The hero sits at a pixel position, not in a cell: he is sixteen across,
 * like the one in stage 09, and drawn here as a plain square, because the
 * picture is not the subject of this stage. */
#define HERO_SIZE 16
#define HERO_STEP 2

static int hero_x = (PICTURE_W - HERO_SIZE) / 2;
static int hero_y = 120;

static uint8_t emulated_frame(void)
{
    uint8_t pad = read_pad();

    if (pad & PAD_LEFT)  hero_x -= HERO_STEP;
    if (pad & PAD_RIGHT) hero_x += HERO_STEP;
    if (pad & PAD_UP)    hero_y -= HERO_STEP;
    if (pad & PAD_DOWN)  hero_y += HERO_STEP;

    /* Keep the hero inside the picture. */
    if (hero_x < 0) hero_x = 0;
    if (hero_y < 0) hero_y = 0;
    if (hero_x > PICTURE_W - HERO_SIZE) hero_x = PICTURE_W - HERO_SIZE;
    if (hero_y > PANEL_H - HERO_SIZE)   hero_y = PANEL_H - HERO_SIZE;

    return pad;
}

static void draw_background(void)
{
    /* A wall of tiles: something for the eye to measure movement against. */
    for (int y = 0; y < 168; y++)
        for (int x = 0; x < PICTURE_W; x++)
            fb[y * PICTURE_W + x] = ((x / 16 + y / 16) & 1) ? COL_BLUE : COL_CYAN;

    fill_rectangle(0, 168, PICTURE_W, PANEL_H - 168, COL_GREEN);
    fill_rectangle(48, 120, 32, 48, COL_MAGENTA);
    fill_rectangle(176, 96, 48, 32, COL_MAGENTA);
}

static void draw_frame(uint8_t pad)
{
    draw_background();

    /* The byte the emulated program received, one square per bit, in the order
     * the bits come out of the register: A, B, Select, Start, up, down, left,
     * right. A lit square means that button reached the program. */
    for (int i = 0; i < 8; i++) {
        uint8_t colour = (pad & (1u << i)) ? COL_WHITE : COL_BLACK;
        fill_rectangle(16 + i * 12, 216, 8, 8, colour);
    }

    /* The hero: a square with one marked corner, so you can see which way he
     * is turned, and red while the blue button is held. */
    fill_rectangle(hero_x, hero_y, HERO_SIZE, HERO_SIZE,
                   (pad & PAD_A) ? COL_RED : COL_YELLOW);
    fill_rectangle(hero_x, hero_y, 4, 4, COL_BLACK);
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();
    input_init();

    for (;;) {
        uint8_t pad = emulated_frame();   /* what the game does this frame */
        draw_frame(pad);                  /* what it wants on the screen    */
        push_picture();                   /* and out to the panel           */
    }
}
