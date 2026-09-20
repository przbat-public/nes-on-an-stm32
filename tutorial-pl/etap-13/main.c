/*
 * Stage 13 — cartridges bigger than the memory the processor can see.
 *
 * Stage 07 opened a game file and read its header, and stage 08 turned the
 * tile data in it into a picture. Both times the whole game sat in memory at
 * once. That works while the game is small. It stops working the moment the
 * game is bigger than the address space, and every game of any size is:
 *
 *   - the processor is a 6502 from 1975. It has sixteen address wires, so it
 *     can name 65,536 bytes and not one more. Part of that space is its own
 *     work memory, part is the picture chip's registers, and what is left is
 *     the cartridge: 32 KB, no matter how big the cartridge really is.
 *   - a game with more levels, more graphics and more music than that does
 *     not fit. Not "fits badly" — does not fit.
 *
 * So the cartridge lies, in a way the processor never notices. The processor
 * still sees 32 KB of cartridge. What is under those addresses changes while
 * the program runs:
 *
 *   $8000-$BFFF   16 KB, and the cartridge decides which 16 KB
 *   $C000-$FFFF   16 KB, always the last 16 KB of the cartridge
 *
 * The switch is one register, and in this cartridge it answers to any write
 * to $8000 or above (stage 08's cartridge ignored those writes entirely).
 * Write a number there and the low half of the cartridge's address space
 * becomes a different piece of the chip. The game does this to reach code and
 * data that do not fit in the half it is currently looking at.
 *
 * Why the top half never moves: the reset vector lives at $FFFC, right at the
 * top. If the top half could be switched away, the processor would not find
 * the address to start at. Every bank-switching cartridge keeps one bank
 * nailed down like that, which is also why the routine that flips the switch
 * lives in it.
 *
 * What to look for on the panel: the picture is drawn by two pieces of
 * program that have never been in memory at the same time. Bank 0 draws the
 * top half of the screen, bank 1 the bottom half, and the game calls one,
 * switches, calls the other, over and over while it runs. Each half uses a
 * different tile, and each bank writes a colour of its own into a palette
 * entry of its own; the two entries stay two apart, which is what the host
 * check in host/host_check.c reads back every frame.
 */
#include <stdint.h>
#include "cpu.h"
#include "ppu.h"

/* ------------------------------------------------------- the panel's wires
 * The same five wires and the same six registers as stage 00. The panel is
 * not what this stage is about, so this part is unchanged: turn on the
 * clocks, set the pins, reset the panel, tell it the colour format.
 */
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

/* ------------------------------------------------------------------ timing
 * One emulated frame is this many instructions. A real console runs about
 * 29,780 processor cycles per frame, but the game in this cartridge spends
 * almost all of its time waiting for the picture chip, so a much smaller
 * budget lands in the same place and leaves the panel more time. Stage 15
 * measures what the emulator actually costs per frame; the number here is a
 * starting point, not a measurement.
 */
#define INSTRUCTIONS_PER_FRAME 20000

/* The colour the panel gets for each palette number the picture chip hands
 * out: black, then eight colours. These are the eight from stage 01. */
static const uint16_t panel_colours[8] = {
    0x0000,   /* 0 black   */
    0xF800,   /* 1 red     */
    0x07E0,   /* 2 green   */
    0x001F,   /* 3 blue    */
    0xFFE0,   /* 4 yellow  */
    0xF81F,   /* 5 magenta */
    0x07FF,   /* 6 cyan    */
    0xFFFF,   /* 7 white   */
};

/* The picture, one byte per pixel, written by the picture chip and read by
 * the send routine. 61,440 bytes is most of this board's 96 KB of RAM, which
 * is the same squeeze the console was in — the reason the framebuffer holds
 * palette numbers instead of colours. */
static uint8_t framebuffer[PPU_W * PPU_H];

/* =====================================================================
 * The cartridge
 * =====================================================================
 * Three 16 KB banks. The first two are the ones the game switches between;
 * the third is the fixed one at the top of the address space.
 *
 * Only the bytes the program and its tables actually use are written out.
 * C fills the rest of every bank with zeros, which is what an empty part of
 * a cartridge really is. Note how little of a bank this game uses, and how
 * much of it a real game would: that difference is the whole reason the
 * switch exists.
 *
 * The bytes are in cartridge.h, and host/asm.py is the program that put them
 * there: there is no cross-assembler in this repository, so the listing in
 * that script is what the game was written in.
 */
#include "cartridge.h"

/* ------------------------------------------------------------- the bus
 * The processor asks for an address; this function decides who answers.
 * This is the memory map of stage 06 with the two halves of the cartridge
 * coming from two different places.
 */
static uint8_t work_ram[0x800];              /* the processor's own 2 KB */
static const uint8_t *cartridge_window;      /* the bank in the window now */
static uint8_t bank;                         /* the bank register itself   */

uint8_t bus_read(uint16_t addr)
{
    if (addr < 0x2000)                       /* the processor's own memory */
        return work_ram[addr & 0x07FF];      /* 2 KB, mirrored, as on the chip */

    if (addr < 0x4000)                       /* the picture chip's registers */
        return ppu_read((uint16_t)(0x2000 + (addr & 7)));

    if (addr >= 0x8000) {                    /* the cartridge */
        if (addr < 0xC000)
            return cartridge_window[addr - 0x8000];
        return bank_fixed[addr - 0xC000];
    }

    return 0;                                /* nothing is wired up here */
}

void bus_write(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) {
        work_ram[addr & 0x07FF] = value;
        return;
    }

    if (addr < 0x4000) {
        ppu_write((uint16_t)(0x2000 + (addr & 7)), value);
        return;
    }

    if (addr < 0x8000)
        return;                              /* no hardware answers here */

    /* A write into the cartridge's own range is not a write to memory: the
     * processors's write line goes to the cartridge, and the cartridge uses
     * it to pick a bank. This is the register the whole stage is about. */
    bank = (uint8_t)(value % CARTRIDGE_BANKS);
    cartridge_window = bank_pointers[bank];
}

/* --------------------------------------------------------- the machine */

static void machine_reset(void)
{
    bank = 0;
    cartridge_window = bank_pointers[0];
    ppu_reset();
    cpu_reset();                             /* reads the reset vector */
}

/* One frame: tell the picture chip a new frame has begun, then let the
 * processor run for this frame's share of instructions. The order matters
 * and it is not a detail: the game's very first act is to wait for a frame
 * to begin, so a flag raised only after the budget would never be seen and
 * the game would wait for ever for something that already happened.
 *
 * The program does not have to finish anything inside one call: whatever it
 * does not finish, it continues next time, which is exactly how a game runs
 * on a console — a frame's work is simply cut off by the next frame. */
static void run_frame(void)
{
    ppu_start_frame();
    for (int i = 0; i < INSTRUCTIONS_PER_FRAME; i++) {
        if (!cpu_step())
            break;                           /* the program stopped itself */
    }
}

static void render_frame(void)
{
    ppu_render_picture(framebuffer);
}

/* -------------------------------------------------------------- the panel */

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
    pin_clear(PIN_RST); delay(80000);
    pin_set(PIN_RST);   delay(400000);

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(800000);
    cmd(CMD_DISPON); delay(160000);
}

/* Send the picture to the panel, 256 pixels of it in the middle and black
 * bars on either side. */
static void push_picture(void)
{
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);

    dc(1); cs(0);
    for (int y = 0; y < PPU_H; y++) {
        const uint8_t *row = &framebuffer[y * PPU_W];
        for (int x = 0; x < PANEL_W; x++) {
            uint16_t colour;
            if (x < PANEL_W / 2 - PPU_W / 2 || x >= PANEL_W / 2 + PPU_W / 2) {
                colour = 0x0000;             /* the bars on either side */
            } else {
                colour = panel_colours[row[x - (PANEL_W / 2 - PPU_W / 2)] & 7];
            }
            spi_byte((uint8_t)(colour >> 8));
            spi_byte((uint8_t)(colour & 0xFF));
        }
    }
    cs(1);
}

int main(void)
{
    gpio_init();
    spi_init();
    panel_init();

    machine_reset();

    for (;;) {
        render_frame();      /* turn video memory into pixels */
        push_picture();      /* send those pixels to the panel */
        run_frame();         /* and let the game draw its next frame */
    }
}
