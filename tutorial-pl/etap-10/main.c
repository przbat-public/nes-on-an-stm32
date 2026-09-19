/*
 * Stage 10 — time: an interrupt, a frame counter, and a steady rhythm.
 *
 * Stages 03 and 09 moved the picture, but they moved it as fast as the loop
 * happened to run: drawing, sending and the pauses between them came out of one
 * sequence of instructions, so every change to the code changed the tempo.
 * Nothing counted time on its own.
 *
 * This stage adds a second clock next to the processor. The timer counts cycles
 * on its own, and every 1,333,333 of them, which is one sixtieth of a second at
 * 80 MHz, it stops the processor where it is, runs a short function of our own,
 * and lets the processor continue. That is an interrupt: a signal from hardware
 * that the program cannot miss and does not have to wait for. Polling asks
 * "has it happened yet?" a million times; an interrupt answers a question the
 * program never asked.
 *
 * Three things come out of that, and they are the three ideas of this stage:
 *
 *   the heartbeat   a counter that the interrupt raises sixty times a second,
 *                   which is the only clock in the program the picture obeys
 *   a short handler an interrupt stops everything else, so the function it runs
 *                   counts and returns; it never draws and never waits
 *   the rhythm      the loop waits for the heartbeat before sending each band,
 *                   so drawing and sending no longer run at whatever speed the
 *                   wire happens to allow
 *
 * Why keeping them apart matters is visible on the panel: the picture is drawn
 * into a framebuffer first and sent afterwards, so nobody ever sees half of one
 * picture and half of the next. Stage 09 drew sprites into that buffer; this
 * stage gives the drawing a tempo, and the picture itself is a plain pattern,
 * because the tempo is the subject here.
 *
 * One honest warning before the code. The panel sits at the end of a serial
 * link that carries about five million bytes a second, and one full 256x240
 * picture is 122,880 bytes, so sixty of them in a second come to more than the
 * wire can take. The emulator lives with the same limit: the console produces
 * sixty frames a second, the panel receives what fits. That is exactly why the
 * two rhythms are separate in the code.
 */
#include <stdint.h>

#define REG32(addr)  (*(volatile uint32_t *)(addr))

/* The clock, the pins and the serial link, as in stage 04. */
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

/* The timer. Registers of the core itself, not of the chip around it, so their
 * addresses start with 0xE000 instead of 0x4000. Three of them make the
 * interval: LOAD holds how many cycles one period lasts, VAL holds how many are
 * left, and CTRL starts the counting and decides where the cycles come from. */
#define SYST_CSR      REG32(0xE000E010UL)
#define SYST_RVR      REG32(0xE000E014UL)
#define SYST_CVR      REG32(0xE000E018UL)

#define SYST_CSR_ENABLE     (1u << 0)   /* count at all                     */
#define SYST_CSR_TICKINT    (1u << 1)   /* call our handler on each period  */
#define SYST_CSR_CLKSOURCE  (1u << 2)   /* count processor cycles, not the
                                         * separate reference clock         */

/* Where the core looks for the interrupt table. A register of the core, like
 * the timer above; the table it points at is a plain array of addresses. */
#define SCB_VTOR      REG32(0xE000ED08UL)

/* Whether the processor accepts interrupts at all. Bit 0 of this register is
 * the mask: set means "everything except the most urgent is on hold". It sits
 * in the core's own block, next to the table that switches single interrupts on
 * one by one; the two functions below are the whole of its use here. */
#define PRIMASK       REG32(0xE000E410UL)

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
#define NES_W     256            /* the picture the console would produce */
#define NES_X     32             /* centred on the wider panel            */
#define BAND_H    8              /* lines per transfer: 4096 bytes        */
#define BAND_COUNT (PANEL_H / BAND_H)

/* The tempo. The core runs at 80,000,000 cycles a second and the console's
 * picture came sixty times a second, so one period is that many cycles:
 *
 *     80,000,000 / 60 = 1,333,333.33
 *
 * We can only write whole cycles into the register, and 1,333,333 is the
 * nearest one below, so the timer runs at 60.000015 Hz. Over a minute that is
 * a thousandth of a frame of drift. Counting sixty such periods gives back a
 * second to within a millisecond, which is close enough for anything the
 * console ever did with time. */
#define CORE_HZ     80000000u
#define FPS         60u
#define TICK_CYCLES (CORE_HZ / FPS)

static uint8_t fb[NES_W * PANEL_H];

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

/* ------------------------------------------------------------------- clocks */

/* 16 MHz (HSI16) x 10 / 2 = 80 MHz, as in stage 04. This has to run before the
 * timer is set up: the timer counts cycles of the core, so its idea of a second
 * is only right once the core runs at the speed the arithmetic above assumes. */
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

/* ---------------------------------------------------------------- the timer
 * Two variables and one function, and the function does almost nothing on
 * purpose. It runs on top of whatever the main loop was doing, so everything it
 * touches has to be finished quickly, and it must leave no trace except those
 * variables. Drawing here would make the tempo depend on how long drawing
 * takes, which is the bug this whole stage exists to remove.
 *
 * The address of this function has to end up in the timer's slot of the
 * interrupt table. The startup file has no weak name in that slot to replace,
 * so vectors_init() below builds the table and points the core at it.
 */
static volatile uint32_t ticks;          /* periods since reset */
static volatile int frame_ready;         /* set by the handler, cleared by the loop */

void SysTick_Handler(void)
{
    ticks++;
    frame_ready = 1;
}

/* The interrupt table is the first thing in program memory: one word per
 * signal, and the core jumps to whatever address the right slot holds. The
 * startup file fills every slot with Default_Handler, which spins for ever,
 * and leaves the timer's slot without a name a C function could take over.
 * Program memory cannot be written at run time, so the table is copied into
 * RAM here, the timer's slot is pointed at our handler, and the core is told
 * where to look. Fifteen is the timer: word zero is the stack pointer, one is
 * reset, and the timer is the last of the core's exceptions. */
#define CORE_VECTORS 16u

extern void Default_Handler(void);       /* the startup file's spinning default */

static uint32_t vector_table[CORE_VECTORS] __attribute__((aligned(128)));

static void vectors_init(void)
{
    for (uint32_t i = 0; i < CORE_VECTORS; i++) {
        vector_table[i] = (uint32_t)Default_Handler;
    }
    vector_table[15] = (uint32_t)SysTick_Handler;
    SCB_VTOR = (uint32_t)vector_table;
}

/* The chip never fires without being told to, so this is where the interval is
 * chosen. RELOAD counts down at one per cycle and the interrupt arrives when it
 * reaches zero, so the period is exactly the number written there over the core
 * frequency. The timer also offers a fixed 10 ms period for people who do not
 * care; we do care, because sixty times a second is not ten milliseconds. */
static void tick_init(void)
{
    SYST_RVR = TICK_CYCLES - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

/* Off and on again, used only around the clear of the flag. Clearing the flag
 * and waiting for it to come back has to look like one step: a tick that lands
 * between the two would be wiped by the clear, and the loop would wait for the
 * period after that one. Two instructions of masking cost nothing. */
static void interrupts_off(void) { PRIMASK = 1; }
static void interrupts_on(void)  { PRIMASK = 0; }

/* Wait for the next period of the timer and say how many have passed in total.
 * The flag is cleared before the wait, not after it: a clear that came after
 * the wait would throw away a tick that arrived while the flag was being read.
 * The empty loop is not decoration: the counter belongs to the handler, so the
 * processor has nothing to do until the flag changes, and reading a variable
 * that the handler writes is enough to wait for it. An idle loop like this is
 * the one place where an interrupt earns its keep. */
static uint32_t wait_for_tick(void)
{
    interrupts_off();
    frame_ready = 0;
    interrupts_on();
    while (!frame_ready) { }
    return ticks;
}

/* -------------------------------------------------------------------- panel */

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
    pin_clear(PIN_RST); delay(400000);
    pin_set(PIN_RST);   delay(2000000);

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(4000000);
    cmd(CMD_DISPON); delay(800000);

    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);   /* black bars, once */
    dc(1); cs(0);
    for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
        spi_byte(0); spi_byte(0);
    }
    cs(1);
}

/* Send one band, and leave the timer alone while doing it. The handler takes a
 * few dozen cycles and touches nothing the transfer uses, and a band is 0.82 ms
 * against a period of 16.67 ms, so there is nothing here worth masking. The
 * pacing lives in wait_for_tick: a flag raised during a transfer is cleared by
 * the next wait, which then waits for the period after it. */
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

/* Diagonal stripes that move without changing shape. Each stripe asks "which
 * band of eight am I in", counting x and y together, and adds the frame number
 * so the answer is one further on with every tick. Nothing is moved: the same
 * arithmetic lands on different colours, and the eye reads that as motion. */
static void draw_pattern(uint32_t frame)
{
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            int stripe = ((x + y + (int)frame) >> 3) & 3;
            fb[y * NES_W + x] = (uint8_t)((stripe == 0) ? 7 : stripe);
        }
    }

    /* A mark that does not follow the stripes, so that a picture which is not
     * moving at all cannot be mistaken for one that is. */
    for (int y = 8; y < 24; y++) {
        for (int x = 8; x < 24; x++) {
            fb[y * NES_W + x] = 7;
        }
    }
}

/* --------------------------------------------------------------------- main
 * Three separate jobs, in the order they happen: wait for the timer, draw the
 * whole picture in memory, send it out band by band. Drawing happens between
 * two ticks rather than during a transfer, which is the reason the picture
 * never flickers: at no moment does the panel hold a mixture of two pictures
 * that were drawn at different times.
 */
int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();
    vectors_init();
    tick_init();

    uint32_t frame;

    for (;;) {
        frame = wait_for_tick();       /* how many periods have gone by */

        draw_pattern(frame);

        for (int i = 0; i < BAND_COUNT; i++) {
            push_band(i * BAND_H);
            (void)wait_for_tick();     /* one band per period, no faster */
        }
    }
}
