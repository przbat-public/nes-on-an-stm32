/*
 * Stage 12 — scrolling and the split screen.
 *
 * Stage 11 could move a sprite, but the world behind it stood still: every
 * pixel of every picture so far was painted at the position it would keep.
 * A game that walks right forever cannot work that way. It would need a
 * picture of the whole level, and one frame of that level is already more
 * memory than the console has.
 *
 * The console solves it with one register. Nothing in the picture moves.
 * The register says where the picture starts, and a value one larger than
 * before shifts the whole view by one pixel. That is scrolling.
 *
 * This program is for your computer, not for the board: the trick lives in
 * the picture chip's timing, and the fastest way to see it is to build the
 * same kind of chip here, where a mistake is a printed line instead of a
 * black panel. Build it with
 *
 *     cc -Wall -Wextra main.c -o etap12 -lz
 *
 * and run it: it writes a picture and checks the timing rules that make the
 * picture correct. The same rules have to hold on the panel later, and a
 * rule written down as a check is a rule you can still trust by then.
 *
 * Three ideas, in the order the program needs them:
 *
 *   the scroll registers   two numbers that say which part of the world is
 *                          drawn in the top-left corner of the screen
 *   the horizontal blank   between two rows of the picture the chip is not
 *                          drawing, so a value written then takes effect on
 *                          the next row and not before
 *   the sprite hit         the flag that tells the program a chosen row has
 *                          just been drawn, which is how a program knows
 *                          when the moment above has arrived
 *
 * The picture that comes out is the thing this stage is for: a status bar
 * that stands still and a world that slides under it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>       /* only for the picture file; see write_png below */

/* ------------------------------------------------------------------ screen
 * The screen is 256 pixels across and 240 down. Both numbers are powers of
 * two and one tile is 8 pixels, so the screen is 32 tiles across and 30
 * down: a nice, round grid, which is exactly why the console uses it.
 */
#define SCREEN_W   256
#define SCREEN_H   240

#define TILE       8
#define MAP_W      (SCREEN_W / TILE)    /* 32 tiles across */

/* Where the status bar ends and the world begins. Two tile rows are enough
 * for two lines of text, and every pixel below them scrolls.
 */
#define SPLIT_ROW  16

/* How fast the world slides, in pixels per frame. One is slow enough to see
 * and fast enough to prove that the picture is moving; the exercises change
 * it.
 */
#define SCROLL_PER_FRAME  1


/* ------------------------------------------------------------------ colours
 * The console has a fixed table of colours and calls them by number, so the
 * picture in memory holds numbers. Storing a colour number and looking up
 * the colour when the pixel is drawn is what makes a whole screen fit in
 * memory: one byte per pixel instead of two.
 *
 * Each number below is one byte holding three levels: three bits of red,
 * three of green and two of blue. White is every bit set, black is none of
 * them, and any colour in between is a mix. The console's own numbers are
 * three times as many and have a table of their own; these keep the program
 * short and the arithmetic visible. Each name has to match what palette_rgb
 * below makes of the byte, or the picture stops looking like its own labels.
 */
#define C_DARK    0x01      /* near black         */
#define C_SKY     0x9B      /* sky blue           */
#define C_GREEN   0x34      /* grass              */
#define C_LIME    0x5C      /* bright grass       */
#define C_BRICK   0xAC      /* brick brown        */
#define C_BROWN   0x64      /* dark brown         */
#define C_WHITE   0xFF      /* white              */
#define C_YELLOW  0xFC      /* yellow             */
#define C_BLUE    0x02      /* darker blue        */

/* One colour number as the three bytes a picture file wants. */
static void palette_rgb(int index, uint8_t *rgb)
{
    uint8_t c = (uint8_t)index;
    rgb[0] = (uint8_t)((c >> 5) & 7);       /* red:   bits 7..5 */
    rgb[1] = (uint8_t)((c >> 2) & 7);       /* green: bits 4..2 */
    rgb[2] = (uint8_t)(c & 3);              /* blue:  bits 1..0 */
    for (int i = 0; i < 3; i++) {
        /* The blue channel has one bit less than the others, so stretch it
         * to the same range. Without that, "all bits set" would come out
         * slightly yellow instead of white. */
        int max_in = (i == 2) ? 3 : 7;
        rgb[i] = (uint8_t)((rgb[i] * 255) / max_in);
    }
}

/* ------------------------------------------------------------------- tiles
 * A tile is eight rows of eight pixels, and every pixel holds a colour
 * number. Four tiles are enough to build a world you can recognise, and a
 * world you recognise is what makes scrolling visible.
 */
#define TILE_SKY    0
#define TILE_BRICK  1
#define TILE_GRASS  2
#define TILE_TREE   3
#define TILE_COUNT  4

static const uint8_t tiles[TILE_COUNT][TILE][TILE] = {
    [TILE_SKY] = {
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
    },
    [TILE_BRICK] = {
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 1, 2, 2, 1, 1, 2, 2 },
        { 1, 1, 2, 2, 1, 1, 2, 2 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 2, 1, 1, 2, 2, 1, 1, 2 },
        { 2, 1, 1, 2, 2, 1, 1, 2 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 1, 2, 2, 1, 1, 2, 2 },
    },
    [TILE_GRASS] = {
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 2, 1, 1, 1, 2, 1, 1 },
        { 1, 1, 1, 1, 1, 1, 1, 2 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 3, 1, 1, 1, 3, 1, 1, 1 },
        { 3, 3, 1, 1, 3, 3, 1, 1 },
        { 3, 3, 3, 1, 3, 3, 3, 1 },
        { 3, 3, 3, 3, 3, 3, 3, 3 },
    },
    [TILE_TREE] = {
        { 0, 0, 1, 1, 1, 1, 0, 0 },
        { 0, 1, 1, 1, 1, 1, 1, 0 },
        { 1, 1, 1, 1, 1, 1, 1, 1 },
        { 0, 1, 1, 1, 1, 1, 1, 0 },
        { 0, 0, 1, 1, 1, 1, 0, 0 },
        { 0, 0, 0, 3, 3, 0, 0, 0 },
        { 0, 0, 0, 3, 3, 0, 0, 0 },
        { 0, 0, 3, 3, 3, 3, 0, 0 },
    },
};

/* A tile's four colours: entry 0 is the backdrop, entries 1 to 3 are the
 * colours the tile's pixels ask for. One tile can therefore use at most
 * three colours plus the backdrop, which is why console art reuses small
 * palettes instead of painting freely.
 */
static const uint8_t tile_palette[TILE_COUNT][4] = {
    [TILE_SKY]   = { C_SKY,   C_SKY,   C_DARK,  C_DARK  },
    [TILE_BRICK] = { C_SKY,   C_BROWN, C_BRICK, C_BROWN },
    [TILE_GRASS] = { C_SKY,   C_LIME,  C_GREEN, C_BROWN },
    [TILE_TREE]  = { C_SKY,   C_GREEN, C_BROWN, C_BROWN },
};

/* ------------------------------------------------------------------ sprites
 * A sprite is a small picture that stands on top of the background and can
 * be put anywhere. The console's sprites are 8 by 8 pixels and up to 64 of
 * them fit on a screen; this one is 16 by 16, drawn as four tiles of eight,
 * which is what the console does when it wants a bigger character.
 *
 * Zero means "no pixel here", so the background shows through. One of the
 * sprites in a frame is the marker whose hit the program waits for, and this
 * is that sprite. Its top row is the last row of the status bar, so the
 * program learns "the bar is finished" the moment the bar is finished. The
 * rest of the sprite hangs below the bar and stays visible.
 */
#define SPRITE_W   16
#define SPRITE_H   16
#define SPRITE_X   120
#define SPRITE_Y   (SPLIT_ROW - 1)      /* row 15, the sprite's rows 15 to 30 */

static const uint8_t sprite[SPRITE_H][SPRITE_W] = {
    { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 },
    { 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 },
    { 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 },
    { 0, 0, 3, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 3 },
    { 0, 0, 3, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 3 },
    { 0, 0, 3, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 3 },
    { 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 },
    { 0, 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0 },
    { 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
};

static const uint8_t sprite_palette[4] = { C_SKY, C_DARK, C_YELLOW, C_BLUE };

/* ------------------------------------------------------------------- world
 * The world is a band of tiles, 32 across and 56 down. It is not a whole
 * level: the level is this band, seen through a window that moves. When the
 * window reaches the edge the picture does not stop, it wraps around to the
 * other side, which is what the console does too.
 */
#define WORLD_TOP   52          /* in tile rows: where the band begins */
#define WORLD_ROWS  56          /* how tall the band is: two screens */
#define GROUND_ROW  62          /* tile row holding the grass surface  */
#define BRICK_ROW   60          /* tile row holding the brick ledge    */

/* The band is taller than the screen on purpose. A band exactly one screen
 * tall would show its own beginning again at the bottom of every frame,
 * which looks like a second copy of the world pasted under the first. */
#define SCROLL_LIMIT  (WORLD_ROWS * TILE - SCREEN_H - SPLIT_ROW)

static uint8_t world[WORLD_ROWS][MAP_W];

static void world_put(int tx, int ty, uint8_t tile)
{
    if (tx >= 0 && tx < MAP_W && ty >= WORLD_TOP
        && ty < WORLD_TOP + WORLD_ROWS) {
        world[ty - WORLD_TOP][tx] = tile;
    }
}

static void world_build(void)
{
    /* Start from empty sky. The checks below build a world of their own and
     * then ask for this one back, and a rebuild has to give the same picture
     * as the first build did. */
    memset(world, TILE_SKY, sizeof world);

    /* Flat ground first: grass on top, brick under it, all the way down.
     * A flat plain is what the eye reads as "the world is moving" when the
     * scroll changes, so everything else is built on top of it. */
    for (int tx = 0; tx < MAP_W; tx++) {
        for (int ty = GROUND_ROW; ty < WORLD_TOP + WORLD_ROWS; ty++) {
            world_put(tx, ty, (ty == GROUND_ROW) ? TILE_GRASS : TILE_BRICK);
        }
    }

    /* A staircase rising out of the plain. Every step is a whole tile, so
     * the terrain stays on the grid the console draws on. */
    static const struct { int tx, row; } steps[] = {
        { 8, GROUND_ROW - 1 }, { 9, GROUND_ROW - 2 },
        { 10, GROUND_ROW - 3 }, { 11, GROUND_ROW - 3 },
        { 12, GROUND_ROW - 2 }, { 13, GROUND_ROW - 1 },
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        for (int ty = steps[i].row; ty < WORLD_TOP + WORLD_ROWS; ty++) {
            world_put(steps[i].tx, ty,
                      (ty == steps[i].row) ? TILE_GRASS : TILE_BRICK);
        }
    }

    /* A ledge of bricks, one tile thick, standing in the air above the
     * plain. It is the landmark that makes a vertical scroll visible: a row
     * of sky, a thin line of bricks, sky again. */
    for (int tx = 14; tx < 19; tx++) {
        world_put(tx, BRICK_ROW, TILE_BRICK);
    }

    /* Two tiles of brick in the row the status bar covers, where the marker
     * sprite stands. Nobody sees them, but they have to be there: the chip
     * sets the hit bit only when a pixel of the marker lands on something
     * the world has painted, and over plain sky there is nothing to land
     * on. */
    world_put(15, WORLD_TOP + 1, TILE_BRICK);
    world_put(16, WORLD_TOP + 1, TILE_BRICK);

    /* Trees stand on the plain. Two tiles side by side read as one crown. */
    world_put(2, GROUND_ROW - 1, TILE_TREE);
    world_put(3, GROUND_ROW - 1, TILE_TREE);
    world_put(20, GROUND_ROW - 1, TILE_TREE);
    world_put(21, GROUND_ROW - 1, TILE_TREE);
    world_put(26, GROUND_ROW - 1, TILE_TREE);
    world_put(27, GROUND_ROW - 1, TILE_TREE);
}

/* ------------------------------------------------------------------- text
 * Five pixels tall, three wide, one pixel of space: the smallest letters
 * that stay readable, and the size a status bar has room for.
 */
static const uint8_t glyphs[][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },   /* 0 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 },   /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 },   /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 },   /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 },   /* 6 */
    { 0x7, 0x1, 0x1, 0x1, 0x1 },   /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 },   /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 },   /* 9 */
    { 0x7, 0x5, 0x7, 0x5, 0x5 },   /* A */
    { 0x7, 0x5, 0x6, 0x5, 0x7 },   /* B */
    { 0x7, 0x4, 0x4, 0x4, 0x7 },   /* C */
    { 0x6, 0x5, 0x5, 0x5, 0x6 },   /* D */
    { 0x7, 0x4, 0x7, 0x4, 0x7 },   /* E */
    { 0x7, 0x4, 0x7, 0x4, 0x4 },   /* F */
    { 0x7, 0x4, 0x5, 0x5, 0x7 },   /* G */
    { 0x5, 0x5, 0x7, 0x5, 0x5 },   /* H */
    { 0x7, 0x2, 0x2, 0x2, 0x7 },   /* I */
    { 0x1, 0x1, 0x1, 0x5, 0x7 },   /* J */
    { 0x5, 0x5, 0x6, 0x5, 0x5 },   /* K */
    { 0x4, 0x4, 0x4, 0x4, 0x7 },   /* L */
    { 0x5, 0x7, 0x7, 0x5, 0x5 },   /* M */
    { 0x6, 0x5, 0x5, 0x5, 0x5 },   /* N */
    { 0x7, 0x5, 0x5, 0x5, 0x7 },   /* O */
    { 0x7, 0x5, 0x7, 0x4, 0x4 },   /* P */
    { 0x7, 0x5, 0x5, 0x7, 0x1 },   /* Q */
    { 0x7, 0x5, 0x7, 0x6, 0x5 },   /* R */
    { 0x7, 0x4, 0x7, 0x1, 0x7 },   /* S */
    { 0x7, 0x2, 0x2, 0x2, 0x2 },   /* T */
    { 0x5, 0x5, 0x5, 0x5, 0x7 },   /* U */
    { 0x5, 0x5, 0x5, 0x5, 0x2 },   /* V */
    { 0x5, 0x5, 0x7, 0x7, 0x5 },   /* W */
    { 0x5, 0x5, 0x2, 0x5, 0x5 },   /* X */
    { 0x5, 0x5, 0x2, 0x2, 0x2 },   /* Y */
    { 0x7, 0x1, 0x2, 0x4, 0x7 },   /* Z */
};

static const char glyph_names[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int glyph_index(char ch)
{
    const char *at = strchr(glyph_names, ch);
    return at ? (int)(at - glyph_names) : -1;
}

/* Write an unsigned number into the text, right to left, and return where
 * it starts. The bar shows numbers, and a console program that wants to show
 * one has to turn it into digits itself: there is no printing on the panel.
 */
static int format_number(char *out, unsigned value)
{
    if (value == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    int at = 0;
    while (value > 0) { out[at++] = (char)('0' + (value % 10)); value /= 10; }
    for (int i = 0; i < at / 2; i++) {
        char t = out[i]; out[i] = out[at - 1 - i]; out[at - 1 - i] = t;
    }
    out[at] = '\0';
    return at;
}

/* ---------------------------------------------------------------- the chip
 * This is the part that matters. Everything above is what the program
 * draws; the structure below is how the console's picture chip turns it
 * into a picture, and the registers in it are the ones the program writes
 * to make the picture slide.
 */
typedef struct {
    /* The scroll registers, as the console keeps them: the horizontal
     * position is kept in two pieces, whole tiles and pixels inside the
     * tile, and the vertical one counts whole pixels. Splitting the
     * horizontal one is what lets the chip start a row in the middle of a
     * tile; the program never touches the pieces, it writes them through
     * scroll_write below. */
    int coarse_x;       /* horizontal position, in whole tiles   */
    int fine_x;         /* horizontal position inside the tile   */
    int pos_y;          /* vertical position, in pixels          */

    /* What the next row will be drawn with. The chip copies the registers
     * into these during the horizontal blank, and the row after the blank
     * is the first row that sees the new value. */
    int line_x;
    int line_y;

    uint8_t sprite_zero_hit;    /* the flag the program waits for  */

    uint8_t framebuffer[SCREEN_H][SCREEN_W];
} ppu_t;

static void ppu_reset(ppu_t *ppu)
{
    memset(ppu, 0, sizeof *ppu);
    ppu->line_y = ppu->pos_y;
}

/* What a program does when it writes the scroll register: it names a
 * position in the world, and the chip splits that number into the parts it
 * keeps, in exactly the order the console's register is written. */
static void scroll_write(ppu_t *ppu, int x, int y)
{
    ppu->coarse_x = (x / TILE) % MAP_W;
    ppu->fine_x   = x % TILE;
    ppu->pos_y    = y % (WORLD_ROWS * TILE);
}

/* One pixel of the world. The column wraps at the screen edge and the row
 * wraps at the end of the band, so a window that runs off one side comes
 * back on the other, exactly as the console's picture does. The colour that
 * came out goes back through colour_out, because the marker's hit is decided
 * by what stands under the sprite. */
static uint8_t world_pixel(const ppu_t *ppu, int px, int py, uint8_t *colour_out)
{
    int wx = (ppu->line_x + px) % SCREEN_W;
    int wy = (ppu->line_y + py) % (WORLD_ROWS * TILE);
    int tx = wx / TILE, ty = wy / TILE;
    uint8_t tile = world[ty][tx];
    uint8_t index = tiles[tile][wy % TILE][wx % TILE];
    uint8_t colour = tile_palette[tile][index & 3];
    if (colour_out) *colour_out = colour;
    return colour;
}

/* Does the marker sprite cover this pixel? A sprite is a small picture
 * drawn on top of the background, and pixel value zero means "nothing
 * here", so the background shows through. */
static int sprite_pixel(int px, int py, uint8_t *colour)
{
    int sx = px - SPRITE_X, sy = py - SPRITE_Y;
    if (sx < 0 || sx >= SPRITE_W || sy < 0 || sy >= SPRITE_H) return 0;
    uint8_t index = sprite[sy][sx];
    if (index == 0) return 0;
    *colour = sprite_palette[index & 3];
    return 1;
}

/* Draw one row. The chip draws a row at a time, left to right, and this is
 * that row: background first, then the sprites on top of it. */
static void ppu_render_row(ppu_t *ppu, int row)
{
    uint8_t background_colour = C_SKY;
    int sprite_over_background = 0;

    for (int px = 0; px < SCREEN_W; px++) {
        uint8_t colour = world_pixel(ppu, px, row, &background_colour);
        uint8_t from_sprite;
        if (sprite_pixel(px, row, &from_sprite)) {
            /* The chip is watching for one thing while it draws: a pixel of
             * the marker sprite landing on a pixel that the world has
             * painted, rather than on empty sky. That one bit is how a
             * program learns which row is being drawn, without counting rows
             * itself. */
            if (background_colour != C_SKY) sprite_over_background = 1;
            colour = from_sprite;
        }
        ppu->framebuffer[row][px] = colour;
    }

    /* Sprite zero is the first sprite of the frame, so its hit is the signal
     * a program can wait for. On the console this flag is set on the spot
     * and stays set until the next frame begins. */
    if (sprite_over_background && SPRITE_Y <= row && row < SPRITE_Y + SPRITE_H) {
        ppu->sprite_zero_hit = 1;
    }
}

/* Draw one row and then close it: the horizontal blank. This is where the
 * chip copies the scroll registers into what the next row will use. A
 * program that writes the register during the blank changes the rest of the
 * picture; a program that writes it in the middle of a row has to wait for
 * the blank anyway.
 */
static void ppu_end_row(ppu_t *ppu, int row)
{
    ppu_render_row(ppu, row);
    ppu->line_x = ppu->coarse_x * TILE + ppu->fine_x;
    ppu->line_y = ppu->pos_y;
}

/* --------------------------------------------------------------- the frame
 * A frame is 240 rows and then a pause: the vertical blank, when the chip
 * draws nothing and the program is free to prepare the next frame. The
 * scroll registers are written at the start of the picture, so that the
 * first row on the screen is the top-left corner of what the program asked
 * for.
 */
static void ppu_start_frame(ppu_t *ppu, int scroll_x, int scroll_y)
{
    ppu->sprite_zero_hit = 0;
    scroll_write(ppu, scroll_x, scroll_y);
    ppu->line_x = ppu->coarse_x * TILE + ppu->fine_x;
    ppu->line_y = ppu->pos_y;
}

/* The program's side of the deal: run the game until the marker sprite has
 * been drawn. A program that writes the scroll register before this moment
 * moves the status bar as well, which is the mistake the last check in this
 * file demonstrates.
 */
static void cpu_wait_for_sprite(ppu_t *ppu, int *row)
{
    while (!ppu->sprite_zero_hit && *row < SCREEN_H) {
        ppu_end_row(ppu, *row);
        (*row)++;
    }
}

/* Draw the status bar. It is not part of the scrolling world: the program
 * draws it again every frame with the scroll at zero, which is why it stands
 * still. The sprite hit is what tells the program when the bar is done.
 *
 * Text on this console is drawn letter by letter from the same kind of table
 * as the scenery: a letter is a tile whose pixels happen to look like a
 * letter. The panel has no idea what a letter is.
 */
static void draw_text(ppu_t *ppu, const char *text, int x, int y)
{
    for (int i = 0; text[i]; i++) {
        int g = glyph_index(text[i]);
        if (g < 0) continue;
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (glyphs[g][row] & (1 << (2 - col))) {
                    ppu->framebuffer[y + row][x + i * 4 + col] = C_WHITE;
                }
            }
        }
    }
}

static void draw_status_bar(ppu_t *ppu, unsigned score, int scroll)
{
    /* A dark strip, then a bright line under it to separate bar from world. */
    for (int y = 0; y < SPLIT_ROW - 1; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            ppu->framebuffer[y][x] = C_DARK;
        }
    }
    for (int x = 0; x < SCREEN_W; x++) {
        ppu->framebuffer[SPLIT_ROW - 1][x] = C_WHITE;
    }

    char digits[8];
    draw_text(ppu, "SCORE", 4, 2);
    format_number(digits, score % 1000000u);
    draw_text(ppu, digits, 28, 2);

    draw_text(ppu, "SCROLL", 4, 9);
    format_number(digits, (unsigned)scroll);
    draw_text(ppu, digits, 32, 9);

    /* How far the world has moved, as a bar. The width is read at a glance,
     * and it is also the easiest thing to check in a test. */
    int filled = (scroll * (SCREEN_W / 2)) / SCREEN_W;
    for (int y = 5; y < 11; y++) {
        for (int x = 0; x < SCREEN_W / 2; x++) {
            ppu->framebuffer[y][80 + x] = (x < filled) ? C_YELLOW : C_WHITE;
        }
    }
}

/* One whole frame, in the order the console does it: get the registers
 * ready, draw the bar, wait for the marker, then hand the rest of the
 * picture to the world at its own scroll position. */
static void draw_frame(ppu_t *ppu, unsigned score, int scroll_x, int scroll_y)
{
    int row = 0;

    ppu_start_frame(ppu, 0, 0);             /* bar: the top of the world */
    cpu_wait_for_sprite(ppu, &row);         /* the bar is done here      */

    draw_status_bar(ppu, score, scroll_x);

    /* The moment has come. Writing the register now changes the rest of
     * this frame and nothing above it: the bar stays where it is. */
    scroll_write(ppu, scroll_x, scroll_y);

    while (row < SCREEN_H) {
        ppu_end_row(ppu, row);
        row++;
    }
}

/* ==================================================================== host
 * Everything below runs on your computer: it turns the frame into a file
 * and checks the rules that make the picture correct.
 */

static int failures;

static void expect(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static unsigned long frame_checksum(const ppu_t *ppu, int first, int last)
{
    unsigned long sum = 2166136261UL;
    for (int y = first; y < last; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            sum = (sum ^ ppu->framebuffer[y][x]) * 16777619UL;
        }
    }
    return sum;
}

/* ------------------------------------------------------------ picture file
 * A PNG file is a signature, then a few named blocks, each with the length
 * of its contents and a check value over them, and inside one block the
 * pixels. The alternative was a file full of raw bytes, which no viewer
 * opens. Only this block of code needs a library, and none of the console
 * ideas live here.
 */
static unsigned long crc_table[256];

static void crc_init(void)
{
    for (unsigned n = 0; n < 256; n++) {
        unsigned long c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc_table[n] = c;
    }
}

static unsigned long crc_of(const unsigned char *p, unsigned long n)
{
    unsigned long c = 0xFFFFFFFFUL;
    for (unsigned long i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFUL;
}

static void be32(unsigned char *out, unsigned long v)
{
    out[0] = (unsigned char)(v >> 24);
    out[1] = (unsigned char)(v >> 16);
    out[2] = (unsigned char)(v >> 8);
    out[3] = (unsigned char)v;
}

static void png_chunk(FILE *f, const char *tag,
                      const unsigned char *data, unsigned long len)
{
    unsigned char header[8], check[4];
    unsigned char *buffer = malloc(len + 4);
    be32(header, len);
    memcpy(header + 4, tag, 4);
    fwrite(header, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    memcpy(buffer, tag, 4);
    if (len) memcpy(buffer + 4, data, len);
    be32(check, crc_of(buffer, len + 4));
    fwrite(check, 1, 4, f);
    free(buffer);
}

static int write_png(const char *path, const ppu_t *ppu)
{
    static const unsigned char signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char header[13];
    unsigned char *raw, *packed;
    uLongf packed_len;
    FILE *f;
    size_t raw_len = (size_t)SCREEN_H * ((size_t)SCREEN_W * 3 + 1);

    raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < SCREEN_H; y++) {
        unsigned char *at = raw + (size_t)y * ((size_t)SCREEN_W * 3 + 1);
        *at++ = 0;                          /* the row's filter: none */
        for (int x = 0; x < SCREEN_W; x++) {
            palette_rgb(ppu->framebuffer[y][x], at);
            at += 3;
        }
    }
    packed_len = compressBound((uLong)raw_len);
    packed = malloc(packed_len);
    if (!packed) { free(raw); return 0; }
    if (compress2(packed, &packed_len, raw, (uLong)raw_len, 9) != Z_OK) {
        free(packed); free(raw); return 0;
    }
    free(raw);

    f = fopen(path, "wb");
    if (!f) { free(packed); return 0; }
    fwrite(signature, 1, 8, f);
    be32(header, SCREEN_W);
    be32(header + 4, SCREEN_H);
    header[8] = 8;      /* eight bits per channel */
    header[9] = 2;      /* colour type 2: red, green and blue, no alpha */
    header[10] = 0; header[11] = 0; header[12] = 0;
    png_chunk(f, "IHDR", header, 13);
    png_chunk(f, "IDAT", packed, (unsigned long)packed_len);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(packed);
    return 1;
}

int main(int argc, char **argv)
{
    static ppu_t ppu;
    unsigned long bar, world_before, world_after;
    int split_row = -1;
    int scroll = 0;
    unsigned score = 0;
    int frames = 0;
    const char *path = "etap12.png";

    if (argc > 1) path = argv[1];

    crc_init();
    world_build();
    ppu_reset(&ppu);

    /* Run the game for a while, the way a real one runs: every frame the
     * player walks one pixel to the right, so the scroll grows and the world
     * follows. The values are fixed so that the checks below always see the
     * same picture. */
    for (frames = 0; frames < 40; frames++) {
        int row = 0;
        scroll += SCROLL_PER_FRAME;
        score += 7;
        if (scroll > SCROLL_LIMIT) scroll = 0;

        ppu_start_frame(&ppu, 0, 0);
        cpu_wait_for_sprite(&ppu, &row);
        if (split_row < 0 && ppu.sprite_zero_hit) split_row = row;

        draw_status_bar(&ppu, score, scroll);
        scroll_write(&ppu, scroll, scroll);

        while (row < SCREEN_H) {
            ppu_end_row(&ppu, row);
            row++;
        }
    }

    /* The frame on disk is the one just drawn: the bar, the marker sprite
     * and the world, with the world at its own scroll. */
    if (!write_png(path, &ppu)) {
        printf("could not write %s\n", path);
        return 1;
    }

    printf("stage 12: scrolling and the split screen\n\n");
    printf("frame %d, score %u, scroll %d pixels, ", frames, score, scroll);
    if (split_row < 0) {
        /* The exercises can move the marker off the tiles it stands on, and
         * then nothing ever fires. Saying so beats printing a row number. */
        printf("the marker never fired\n");
    } else {
        printf("marker fired at row %d\n", split_row);
    }
    printf("picture written to %s\n\n", path);

    bar = frame_checksum(&ppu, 0, SPLIT_ROW);
    world_before = frame_checksum(&ppu, SPLIT_ROW, SCREEN_H);

    /* Check 1: the marker tells the program which row is being drawn. */
    {
        ppu_t probe;
        int row = 0;
        ppu_reset(&probe);
        ppu_start_frame(&probe, 0, 0);
        while (row < SPRITE_Y) { ppu_end_row(&probe, row); row++; }
        expect(probe.sprite_zero_hit == 0, "no hit before the marker's row");
        while (row < SPRITE_Y + SPRITE_H) { ppu_end_row(&probe, row); row++; }
        expect(probe.sprite_zero_hit == 1, "hit once the marker has been drawn");
    }

    /* Check 2: the bar keeps its place while the world moves on. The bar
     * changes between frames only because the score and the scroll bar in it
     * change; the world below slides by the scroll. */
    draw_frame(&ppu, score + 7, scroll + SCROLL_PER_FRAME, scroll + 1);
    world_after = frame_checksum(&ppu, SPLIT_ROW, SCREEN_H);
    expect(world_after != world_before, "the world moved by one pixel");
    expect(ppu.framebuffer[0][0] == C_DARK && ppu.framebuffer[3][4] == C_WHITE,
           "the bar still starts in the top-left corner");

    /* Check 3: the world is one pixel further along, not merely different.
     * Any two pictures differ; this one asks whether the second picture is
     * the first one read from eight columns later. That can only hold where
     * the world repeats, so the strip below is built to repeat: rows of
     * grass standing in sky. It is read below the sprite, because a sprite
     * covers the same place in both frames and would hide the shift. */
    for (int ty = 0; ty < WORLD_ROWS; ty++) {
        int strip = (ty == 1 || ty >= 6);
        for (int tx = 0; tx < MAP_W; tx++) {
            world[ty][tx] = (uint8_t)(strip ? TILE_GRASS : TILE_SKY);
        }
    }
    {
        ppu_t a, b;
        int test_row = 48;                  /* world row 6, whole tiles */
        ppu_reset(&a); ppu_reset(&b);
        draw_frame(&a, score, 8, 0);
        draw_frame(&b, score, 16, 0);
        /* Screen column x at scroll 8 shows world column 8 + x, and screen
         * column x + 8 at scroll 16 shows world column 24 + x. Both land on
         * the same pixel of a tile of the same kind, so the two pixels must
         * be equal. A scroll that moved by nine pixels, or by two, fails. */
        int same = 1, compared = 0;
        for (int x = 0; x < SCREEN_W - TILE; x++) {
            if ((8 + x) % TILE != (24 + x) % TILE) continue;
            compared++;
            if (a.framebuffer[test_row][x] != b.framebuffer[test_row][x + TILE]) {
                same = 0;
            }
        }
        expect(compared > 200 && same, "one tile of scroll is one tile of world");
    }
    world_build();                          /* back to the picture on disk */

    /* Check 4: the mistake. A program that writes the scroll register
     * before the bar is drawn scrolls the bar too, which is the difference
     * between a status bar and a bar that slid off the screen. The frame
     * goes to a second file so the reader can put the two pictures side by
     * side. */
    {
        ppu_t careless;
        char broken[512];
        snprintf(broken, sizeof broken, "%s", path);
        if (strstr(broken, ".png")) {
            strcpy(strstr(broken, ".png"), "-niecierpliwy.png");
        } else {
            strcpy(broken + strlen(broken), "-niecierpliwy");
        }

        ppu_reset(&careless);
        /* Everything at once: the scroll set, the bar drawn, nothing waited
         * for. */
        ppu_start_frame(&careless, scroll, scroll);
        draw_status_bar(&careless, score, scroll);
        for (int row = 0; row < SCREEN_H; row++) ppu_end_row(&careless, row);

        expect(frame_checksum(&careless, 0, SPLIT_ROW) != bar,
               "written too early, the scroll moves the bar too");
        if (!write_png(broken, &careless)) {
            printf("could not write %s\n", broken);
            return 1;
        }
        printf("  the impatient frame is in %s\n", broken);
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
