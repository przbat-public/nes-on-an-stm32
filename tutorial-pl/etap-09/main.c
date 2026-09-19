/*
 * Stage 9 — sprites: small pictures that move over the background.
 *
 * Stage 08 built the background out of 8x8 tiles laid in a grid. Everything
 * in that picture sits on the grid: a tile is either in a cell or it is not,
 * and a thing that should stand between two cells cannot be drawn at all.
 *
 * This stage adds the second half of the console's picture hardware, and it
 * rests on two ideas:
 *
 *   a sprite   a small picture of its own, carried in its own table, with a
 *              position measured in single pixels rather than in cells
 *   drawing    one picture is not stored, it is composed. The background
 *   order      goes down first, then the sprites are laid on top one after
 *              another, and whatever is drawn last is what the eye sees.
 *
 * Moving a sprite therefore costs nothing: change its position, compose the
 * picture again, and send it. That is the whole animation on this stage, one
 * pixel per frame. When it happens is stage 10's problem; here it happens as
 * fast as the loop can go.
 *
 * Kept from stage 04: the picture is composed in RAM and leaves the chip in
 * bands, so the panel never waits for a whole frame before it starts drawing.
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
#define NES_W     256             /* the emulated picture, centred in the panel */
#define NES_X     32
#define BAND_H    8               /* scanlines per SPI transfer */

/* A number in the picture is not a colour, it is the number of a colour. The
 * panel wants sixteen bits per pixel; that is twice what the picture costs,
 * so the panel gets the colours and the picture keeps the numbers. Sprites
 * read the same table, and number 0 in a sprite is the one number a sprite
 * may not use: it means "no pixel here", so the background shows through. */
#define COLOUR_COUNT 8
static const uint16_t palette[COLOUR_COUNT] = {
    0x001F,   /* 0 the panel at its brightest blue                  */
    0xFFFF,   /* 1 white   cloud, highlight, the hero's eyes        */
    0xF800,   /* 2 red     the hero's tunic                         */
    0xFFE0,   /* 3 yellow  the hero's belt                          */
    0x07E0,   /* 4 green   grass, and the shell of the walking bug  */
    0x0000,   /* 5 black   outline, and the dark side of the trunk  */
    0xF81F,   /* 6 magenta the bug's feet                           */
    0xFCE0,   /* 7 sand    the path, and the sunlit side of the trunk */
};

/* ------------------------------------------------------ talking to the panel
 * Five wires, one byte at a time. Unchanged from stage 04 and not what this
 * stage is about, so read it once and treat it as machinery. */
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

/* ------------------------------------------------------------- the background
 * A tile is eight rows of eight pixels, so the table is a function of two
 * numbers: which tile, and which row of it. Colour 0 is the sky, which is
 * exactly how the console makes a hole in a tile: one shared colour stands
 * for "nothing was drawn here".
 */
#define TILE_ROWS 8

#define TILE_SKY    0
#define TILE_CLOUD  1
#define TILE_GRASS  2
#define TILE_PATH   3
#define TILE_BUSH   4
#define TILE_BLOCK  5
#define TILE_TRUNK  6

static const uint8_t tiles[][TILE_ROWS][8] = {
    /* 0 sky: a plain square */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    /* 1 cloud */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 1, 1, 1, 0, 0, 0 },
        { 0, 1, 1, 1, 1, 1, 0, 0 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 0, 1, 1, 1, 1, 1, 1, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    /* 2 grass */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 4, 0, 0, 0 },
        { 0, 0, 0, 4, 4, 0, 0, 0 },
        { 0, 0, 4, 4, 4, 0, 0, 4 },
        { 4, 0, 4, 4, 4, 0, 4, 4 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
    },
    /* 3 path */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 7, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 7, 0, 0 },
        { 7, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 7, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    /* 4 bush */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 4, 4, 0, 0, 0 },
        { 0, 0, 4, 4, 4, 4, 0, 0 },
        { 0, 4, 4, 4, 4, 4, 4, 0 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
        { 4, 4, 4, 4, 4, 4, 4, 4 },
    },
    /* 5 block: one of the console's standard bricks */
    {
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 2, 2, 2, 2, 2, 2, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
    },
    /* 6 trunk: the tree the hero walks behind, cut into two rows of tiles */
    {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
        { 5, 5, 5, 5, 5, 5, 5, 5 },
    },
};

#define TILE_COUNT  (sizeof(tiles) / sizeof(tiles[0]))
#define TILE_CELLS  8
#define BG_COLS     (NES_W / TILE_CELLS)          /* 32 cells across */
#define BG_ROWS     (PANEL_H / TILE_CELLS)        /* 30 cells down   */

/* The background is a grid of tile numbers, exactly like stage 08. The sky
 * fills it, and the scene overwrites the cells it cares about. */
static uint8_t bg[BG_ROWS][BG_COLS];

/* One tile of the background into one row of the picture. The index is
 * spelled out here rather than hidden in a pointer: this line is where "a
 * cell holds a tile number" turns into pixels. */
static void bg_tile_row(uint8_t *row, int count, int tx, int ty, int tile_row)
{
    const uint8_t *pixels = tiles[bg[ty][tx]][tile_row];
    for (int i = 0; i < TILE_CELLS; i++) {
        row[count + i] = pixels[i];
    }
}

static void bg_fill(int col0, int row0, int col1, int row1, uint8_t tile)
{
    for (int ty = row0; ty < row1; ty++) {
        for (int tx = col0; tx < col1; tx++) {
            bg[ty][tx] = tile;
        }
    }
}

/* ------------------------------------------------------------- the sprites
 * A bitmap is a small picture in a table, one number per pixel, and it is
 * carried around by a position that is measured in pixels. Nothing here says
 * what the picture means; the numbers land in the same colour table the
 * background uses, and that is why a sprite can share it.
 *
 * 16 by 16 is a game-design choice, not a hardware one: the console draws
 * sprites of 8 by 8 or 8 by 16, and a bigger character is stitched together
 * from those. The hero below is four such squares, kept in one table so the
 * reader sees one picture rather than four.
 */
#define HERO_W     16
#define HERO_H     16
#define HERO_FRAMES 2

/* Two frames of the same walk. He faces right — the sword hand and the eyes
 * are both on that side — and only the last three rows differ between the
 * frames, which is all an eye needs to read a walk. */
static const uint8_t hero_bitmaps[HERO_FRAMES][HERO_H][HERO_W] = {
    /* 0: legs apart */
    {
        { 0,0,0,0,0,0,5,5,5,5,0,0,0,0,0,0 },
        { 0,0,0,0,0,5,1,1,1,1,1,5,0,0,0,0 },
        { 0,0,0,0,0,5,2,2,2,2,5,0,0,0,0,0 },
        { 0,0,0,0,0,5,2,2,2,2,5,0,0,0,0,0 },
        { 0,0,0,0,0,5,5,5,5,5,5,0,0,0,0,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,0,0,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,5,5,5,5 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,3,3,3,3,3,3,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,0,5,5,5,5,5,5,0,0,1,5,0 },
        { 0,0,0,0,0,3,3,3,3,3,3,0,0,0,0,0 },
        { 0,0,0,0,3,3,3,0,0,3,3,3,0,0,0,0 },
        { 0,0,0,3,3,3,0,0,0,0,3,3,3,0,0,0 },
    },
    /* 1: legs together, same body and the same sword hand */
    {
        { 0,0,0,0,0,0,5,5,5,5,0,0,0,0,0,0 },
        { 0,0,0,0,0,5,1,1,1,1,1,5,0,0,0,0 },
        { 0,0,0,0,0,5,2,2,2,2,5,0,0,0,0,0 },
        { 0,0,0,0,0,5,2,2,2,2,5,0,0,0,0,0 },
        { 0,0,0,0,0,5,5,5,5,5,5,0,0,0,0,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,0,0,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,5,5,5,5 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,3,3,3,3,3,3,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,5,2,2,2,2,2,2,5,0,1,5,0 },
        { 0,0,0,0,0,5,5,5,5,5,5,0,0,1,5,0 },
        { 0,0,0,0,0,0,3,3,3,3,0,0,0,0,0,0 },
        { 0,0,0,0,0,3,3,0,0,3,3,0,0,0,0,0 },
        { 0,0,0,0,3,3,3,0,0,3,3,3,0,0,0,0 },
    },
};

#define BUG_W   16
#define BUG_H   16

static const uint8_t bug[BUG_H][BUG_W] = {
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,4,4,4,4,0,0,0,0,0,0 },
    { 0,0,0,0,4,4,4,4,4,4,4,4,0,0,0,0 },
    { 0,0,0,0,4,4,4,4,4,4,4,4,0,0,0,0 },
    { 0,0,0,4,4,4,4,4,4,4,4,4,4,0,0,0 },
    { 0,0,0,4,4,1,1,4,4,1,1,4,4,0,0,0 },
    { 0,0,0,4,4,1,1,4,4,1,1,4,4,0,0,0 },
    { 0,0,0,4,4,4,4,4,4,4,4,4,4,0,0,0 },
    { 0,0,0,4,4,4,4,4,4,4,4,4,4,0,0,0 },
    { 0,0,0,0,4,4,4,4,4,4,4,4,0,0,0,0 },
    { 0,0,0,0,6,6,0,0,0,0,6,6,0,0,0,0 },
    { 0,0,0,6,6,0,0,0,0,0,0,6,6,0,0,0 },
    { 0,0,6,6,0,0,0,0,0,0,0,0,6,6,0,0 },
};

/* The trunk is 16 wide as well, so it can hide the hero completely. The
 * console would call this a column of background, but a plain picture with a
 * position is simpler and shows the same thing. */
#define TRUNK_W 16
#define TRUNK_H 40

static const uint8_t trunk[TRUNK_H][TRUNK_W] = {
    { 0,0,5,5,5,5,5,5,5,5,5,5,5,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,7,5,5,7,7,7,7,5,5,7,5,0,0 },
    { 0,0,5,7,7,7,7,7,7,7,7,7,7,5,0,0 },
    { 0,0,5,5,7,7,7,7,7,7,7,7,5,5,0,0 },
    { 0,0,5,7,7,5,5,5,5,5,5,7,7,5,0,0 },
    { 0,0,5,5,5,5,5,5,5,5,5,5,5,5,0,0 },
};

/* A sprite is a position plus a picture. That is the whole type, and it is
 * why a game can move a thing by writing two numbers. */
typedef struct {
    int x;                 /* pixels, not cells: a sprite sits anywhere */
    int y;
    const uint8_t *pixels; /* HERO_H rows of HERO_W numbers, row by row   */
    int w;                 /* how many of those numbers make a row        */
    int h;                 /* how many rows there are                     */
} sprite_t;

#define SPRITE_COUNT 3
#define HERO_INDEX   0         /* where the hero stands in the list       */
#define GROUND_Y     216       /* row 27 of the grid: the first grass row */
#define HERO_STEP    1         /* pixels per frame, and no more           */
#define HERO_LIMIT   (NES_W - HERO_W)
#define WALK_MASK    7         /* change the feet every 8 frames          */

/* The list. Three things live in it: the hero, the tree and the bug. The
 * order is the answer to "what covers what" — whoever stands first in this
 * list is drawn first, and everything after him is laid on top. The tree is
 * second, so it hides the hero when he walks past it. */
static sprite_t sprites[SPRITE_COUNT] = {
    { 16,  GROUND_Y - HERO_H,  &hero_bitmaps[0][0][0], HERO_W, HERO_H }, /* the hero */
    { 128, GROUND_Y - TRUNK_H, &trunk[0][0], TRUNK_W, TRUNK_H }, /* the tree */
    { 200, GROUND_Y - BUG_H,   &bug[0][0],   BUG_W,  BUG_H  },   /* the bug  */
};

/* ------------------------------------------------------------- the picture
 * One byte per pixel, holding a colour number: 256 by 240 is 61,440 bytes,
 * which is the one large thing this program keeps in memory. */
static uint8_t picture[NES_W * PANEL_H];

/* Compose the background into the picture. Every frame starts here, so the
 * sprites of the previous frame are wiped out and drawn again in their new
 * places; nothing has to be erased by hand. */
static void compose_background(void)
{
    for (int y = 0; y < PANEL_H; y++) {
        uint8_t *row = &picture[y * NES_W];
        int ty = y / TILE_CELLS;
        for (int tx = 0; tx < BG_COLS; tx++) {
            bg_tile_row(row, tx * TILE_CELLS, tx, ty, y % TILE_CELLS);
        }
    }
}

/* Lay one sprite over whatever is already in the picture. Two rules, and
 * both matter:
 *
 *   - a pixel whose number is 0 leaves the picture alone, so the background
 *     shows through the gaps in the picture,
 *   - a pixel that is drawn stays, so the next sprite in the list can be
 *     hidden behind this one.
 *
 * Positions may be negative or past the right edge; the loop clips instead
 * of the caller, because a sprite half off the screen is normal.
 */
static void compose_sprite(const sprite_t *s)
{
    for (int sy = 0; sy < s->h; sy++) {
        int y = s->y + sy;
        if (y < 0 || y >= PANEL_H) {
            continue;
        }
        const uint8_t *src = &s->pixels[sy * s->w];
        uint8_t *row = &picture[y * NES_W];
        for (int sx = 0; sx < s->w; sx++) {
            int x = s->x + sx;
            if (x < 0 || x >= NES_W) {
                continue;
            }
            uint8_t colour = src[sx];
            if (colour != 0) {       /* 0 is not a colour: it is a hole */
                row[x] = colour;
            }
        }
    }
}

/* The list, in order, from the bottom of the picture upwards. */
static void compose_sprites(void)
{
    for (int i = 0; i < SPRITE_COUNT; i++) {
        compose_sprite(&sprites[i]);
    }
}

/* ------------------------------------------------- send the picture to the panel
 * Bands, as in stage 04: one strip of scanlines at a time, colours instead of
 * numbers, and the next band can be composed while this one travels. */
static void push_band(int y0)
{
    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    for (int y = 0; y < BAND_H; y++) {
        const uint8_t *row = &picture[(y0 + y) * NES_W];
        for (int x = 0; x < NES_W; x++) {
            uint16_t colour = palette[row[x] & (COLOUR_COUNT - 1)];
            spi_byte((uint8_t)(colour >> 8));
            spi_byte((uint8_t)(colour & 0xFF));
        }
    }
    cs(1);
}

static void push_picture(void)
{
    for (int y = 0; y < PANEL_H; y += BAND_H) {
        push_band(y);
    }
}

/* ------------------------------------------------------------ the board
 * As in stage 04: the chip starts on its internal 4 MHz clock, which is far
 * too slow to compose and send a picture; 16 MHz x 10 / 2 gives 80 MHz. */
static void clock_init(void)
{
    FLASH_ACR = 4u | (1u << 8) | (1u << 9) | (1u << 10);  /* wait states + caches */

    RCC_CR |= (1u << 8);                       /* HSI16 on */
    while (!(RCC_CR & (1u << 10))) { }

    RCC_PLLCFGR = (2u << 0)                    /* source: HSI16             */
                | (10u << 8)                   /* N = 10 -> 160 MHz VCO     */
                | (1u << 24);                  /* R = 2  -> 80 MHz, enabled */
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

    /* BR = 0: the clock is the peripheral clock divided by two, 40 MHz. */
    SPI1_CR1 = (1u << 2) | (1u << 8) | (1u << 9);
    SPI1_CR2 = (7u << 8);
    SPI1_CR1 |= (1u << 6);
}

static void panel_init(void)
{
    pin_clear(PIN_RST); delay(400000);         /* 400k loops at 80 MHz ~ 20 ms */
    pin_set(PIN_RST);   delay(2000000);        /* ~120 ms */

    cmd(CMD_COLMOD); data(0x55);
    cmd(CMD_MADCTL); data(0x60);
    cmd(CMD_SLPOUT); delay(4000000);
    cmd(CMD_DISPON); delay(800000);

    /* Paint the side bars black once. They are outside the picture the
     * console produces, and nothing will ever draw in them again. */
    set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    dc(1); cs(0);
    for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
        spi_byte(0); spi_byte(0);
    }
    cs(1);
}

/* ---------------------------------------------------------------- the scene
 * One screen of background, written once at the start: the sky everywhere,
 * then the ground, the path, two clouds, a bush and a block. After this the
 * background never changes, and only the sprite list moves. */
static void build_background(void)
{
    bg_fill(0, 0, BG_COLS, BG_ROWS, TILE_SKY);

    bg_fill(0, 27, BG_COLS, BG_ROWS, TILE_GRASS);   /* rows 216 and below */
    bg_fill(0, 28, BG_COLS, BG_ROWS, TILE_PATH);    /* the path they stand on */

    bg_fill(3, 3, 6, 5, TILE_CLOUD);
    bg_fill(20, 6, 23, 8, TILE_CLOUD);
    bg_fill(8, 26, 10, 27, TILE_BUSH);
    bg_fill(28, 25, 30, 27, TILE_BLOCK);
    bg_fill(29, 26, 30, 27, TILE_BLOCK);
}

/* Move the hero one pixel and change the feet every few frames. There is no
 * clock in this stage: a frame is one pass of the loop, so "every 8 frames"
 * means "every 8 passes". If the loop ran on a slower chip the walk would
 * simply be slower. Stage 10 fixes that. */
static void step_hero(void)
{
    static int direction = HERO_STEP;
    static uint32_t frames;
    sprite_t *hero = &sprites[HERO_INDEX];

    hero->x += direction;
    if (hero->x <= 0) {
        hero->x = 0;
        direction = HERO_STEP;
    } else if (hero->x >= HERO_LIMIT) {
        hero->x = HERO_LIMIT;
        direction = -HERO_STEP;
    }

    frames++;
    if (frames & WALK_MASK) {
        hero->pixels = &hero_bitmaps[0][0][0];
    } else {
        hero->pixels = &hero_bitmaps[1][0][0];
    }
}

static void draw_frame(void)
{
    compose_background();     /* first the background ...            */
    compose_sprites();        /* ... then the sprites, in list order */
    push_picture();
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    panel_init();

    build_background();             /* the scene is written once ... */
    sprites[HERO_INDEX].y = GROUND_Y - HERO_H;   /* ... the hero stands on it */

    for (;;) {
        draw_frame();
        step_hero();
    }
}
