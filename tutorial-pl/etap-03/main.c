/*
 * Stage 3 — time: the clock, an endless loop, and a delay in milliseconds.
 *
 * Stages 1 and 2 left the chip on its power-on clock, about 4 MHz, and drew one
 * picture that then sat there forever. This stage does two things:
 *
 *   1. it raises the clock to 80 MHz. The chip has an internal 16 MHz
 *      oscillator; a circuit called a PLL multiplies and divides that number up
 *      to 80 MHz, and the core is then switched over to it. Stage 00 promised
 *      this, and clock_init() below is where the promise is kept.
 *
 *   2. it never stops. main() ends in an endless loop that redraws the picture
 *      and then waits, so the panel finally shows something that moves. The
 *      waiting is done by a delay in milliseconds, which is what turns "this
 *      happened after that" into "this happened twenty milliseconds later".
 *
 * Everything up to clock_init() is new. The panel code below it is stage 02,
 * moved around but not changed, and the framebuffer from stage 02 is what the
 * moving bar is drawn into.
 */
#include <stdint.h>

/* ------------------------------------------------------------------ registers
 * A register is a 32-bit word at a fixed address. Write a number into it and
 * the piece of hardware that lives at that address changes what it does. The
 * addresses below are the only ones this stage needs.
 */
#define REG32(addr)  (*(volatile uint32_t *)(addr))

#define FLASH_ACR     REG32(0x40022000UL + 0x00)  /* how slow the core must read flash   */
#define RCC_CR        REG32(0x40021000UL + 0x00)  /* which oscillators are running       */
#define RCC_CFGR      REG32(0x40021000UL + 0x08)  /* which one feeds the core            */
#define RCC_PLLCFGR   REG32(0x40021000UL + 0x0C)  /* what the PLL multiplies and divides */
#define RCC_AHB2ENR   REG32(0x40021000UL + 0x4C)  /* GPIO clock enable                   */
#define RCC_APB2ENR   REG32(0x40021000UL + 0x60)  /* SPI1 clock enable                   */
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

#define COLOUR_COUNT 8
static const uint16_t palette[COLOUR_COUNT] = {
    0x0000,   /* 0 black */
    0xF800,   /* 1 red   */
    0x07E0,   /* 2 green */
    0x001F,   /* 3 blue  */
    0xFFE0,   /* 4 yellow */
    0xF81F,   /* 5 magenta */
    0x07FF,   /* 6 cyan  */
    0xFFFF,   /* 7 white */
};

/* The picture, one byte per pixel, exactly as in stage 02. The bytes are colour
 * numbers; palette[] turns a number into a colour on the way out. */
static uint8_t framebuffer[PANEL_W * PANEL_H];

/* ---------------------------------------------------------------- the picture
 * A red bar, BAR_W pixels wide and as tall as the panel, sliding to the right.
 */
#define BAR_W        24      /* how wide the moving bar is, in pixels */
#define PIXELS_PER_FRAME 4   /* how far it moves in one turn of the loop */

/* Everything is drawn at black first, so nothing is left over from the frame
 * before it. The bar is then drawn on top at the position this frame asks for.
 * Every pixel of the picture is written in every frame: that is the simplest
 * rule that cannot leave rubbish on the screen. */
static void draw_frame(int bar_x)
{
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            uint8_t colour = 0;
            if (x >= bar_x && x < bar_x + BAR_W) {
                colour = 1;               /* red */
            }
            framebuffer[y * PANEL_W + x] = colour;
        }
    }

    /* A green marker in a fixed corner. Nothing moves it, so if the picture on
     * the panel ever comes out rotated or mirrored, this is what shows it. */
    for (int y = PANEL_H - 20; y < PANEL_H; y++) {
        for (int x = PANEL_W - 20; x < PANEL_W; x++) {
            framebuffer[y * PANEL_W + x] = 2;
        }
    }
}

/* ------------------------------------------------------------------- helpers */
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

/* ------------------------------------------------------------------- the clock
 * The chip wakes up on a slow internal oscillator, about 4 MHz: it needs no
 * crystal and no wiring, and it is why the picture in stage 00 took a visible
 * moment to fill. This stage switches on a second internal oscillator, HSI16,
 * which runs at 16 MHz, and feeds that to a circuit called the PLL. The PLL
 * multiplies and divides the number up to 80 MHz, and the core is then switched
 * over to it. The target is 80 MHz, twenty times more work per second, and the
 * four steps have to happen in this order.
 *
 * The PLL is built from three numbers, and the register holds all three at once:
 *
 *     M   divides the source by 1      ->  16 MHz into the PLL
 *     N   multiplies that by 10        -> 160 MHz inside the PLL
 *     R   divides that by 2            ->  80 MHz out
 *
 * The tick is the beat the whole chip marches to: one tick, one step of work.
 * At 4 MHz there are four million steps in a second, at 80 MHz there are eighty
 * million, and that is the difference between a picture that takes a visible
 * moment to fill and one that can be redrawn many times a second.
 */
static void clock_init(void)
{
    /* Step 1: tell the flash memory to take its time.
     * The core runs at 80 MHz, one tick every 12.5 ns, and flash memory cannot
     * be read that fast. Four wait states add four ticks to every read, which is
     * what the memory needs to answer at all. The three bits above them switch
     * on the small instruction and data caches, so most reads never leave the
     * core and the wait states are paid only now and then. */
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);

    /* Step 2: start the internal 16 MHz oscillator (HSI16). The bit goes up
     * immediately, but the oscillator needs a moment before its ticks are worth
     * anything, so the status bit next to it is the one to wait for. */
    RCC_CR |= (1u << 8);                       /* HSI16 on */
    while (!(RCC_CR & (1u << 10))) { }         /* wait until it is steady */

    /* Step 3: describe the PLL, then start it.
     * The fields sit in one register, each in its own group of bits, which is
     * why they are written as shifts: (10 << 8) means "the number 10, placed
     * eight bits up". The values are not always the numbers from the diagram:
     * the M field holds M - 1, the R field holds R/2 - 1, and only the N field
     * holds the number itself. The two lowest bits pick the source, and one more
     * bit has to be set for the R output to leave the PLL at all. Everything
     * below is what has to be written for HSI16, M = 1, N = 10 and R = 2.
     *
     * Waiting for the PLL is not optional: until it locks, its output is not the
     * frequency we asked for. */
    RCC_PLLCFGR = (2u << 0)                    /* bits 1:0 = 10, HSI16 selected  */
                | (0u << 4)                    /* M = 1  -> 16 MHz into the PLL  */
                | (10u << 8)                   /* N = 10 -> 160 MHz inside it    */
                | (0u << 25)                   /* R = 2  -> 80 MHz out           */
                | (1u << 24);                  /* R output enabled               */
    RCC_CR |= (1u << 24);                      /* PLL on */
    while (!(RCC_CR & (1u << 25))) { }         /* wait until it is locked */

    /* Step 4: hand the core over to the PLL.
     * After this write the chip is running at 80 MHz, and the two bits that
     * report the current source are read back to confirm the switch happened. */
    RCC_CFGR = (3u << 0);                      /* the core now runs on the PLL */
    while ((RCC_CFGR & (3u << 2)) != (3u << 2)) { }   /* wait for the switch */
}

/* -------------------------------------------------------------------- the time
 * A delay in milliseconds. Nothing here knows what a millisecond is. The core
 * can count its own steps and nothing else, so the count below comes from
 * looking at the handful of instructions the compiler turns the inner loop
 * into and working out how long one turn of it takes at 80 MHz.
 *
 * That makes the delay approximate, and it says so. The true length depends on
 * what else the chip is doing, on how long a read from flash really takes, and
 * on how long one instruction really takes once the caches are in the way. Only
 * a circuit that counts ticks of the clock itself can measure time exactly, and
 * the chapter comes back to that when the emulator needs a steady frame rate.
 */
#define LOOPS_PER_MS  20000u

static void delay_ms(uint32_t ms)
{
    while (ms--) {
        for (volatile uint32_t i = 0; i < LOOPS_PER_MS; i++) { }
    }
}

/* ----------------------------------------------------------------- send a frame
 * The bytes in the framebuffer are colour numbers. The panel wants colours, so
 * each byte is looked up in the palette and sent as two bytes, high one first.
 */
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

/* -------------------------------------------------------------------- the panel */

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

    /* Now that the core runs at 80 MHz this bus runs at 40 MHz: the divider is
     * still two, but two of a much bigger number. */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);
}

/* The panel needs time as well, and now the waiting is written in milliseconds
 * instead of in counts of a loop. These numbers come from its datasheet. */
static void panel_init(void)
{
    pin_clear(PIN_RST); delay_ms(20);
    pin_set(PIN_RST);   delay_ms(120);

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay_ms(120);
    cmd(CMD_DISPON); delay_ms(20);
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();

    /* The loop that never ends. Drawing one picture is not enough any more: the
     * bar has to be somewhere new every time round, so the loop keeps a count of
     * how many turns it has made and works the position out from that count.
     * There is nothing after the loop, so it runs until the power goes off. */
    uint32_t frame = 0;

    for (;;) {
        int bar_x = (int)((frame * PIXELS_PER_FRAME) % PANEL_W);

        draw_frame(bar_x);
        push_framebuffer();

        /* Wait before moving on. Without this the bar would move as fast as the
         * screen can be redrawn, which is not a speed anyone can follow. */
        delay_ms(20);

        frame++;
    }
}
