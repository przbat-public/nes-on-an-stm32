/*
 * Stage 14 — the cartridge chip: a split screen, palettes in the middle of a
 * line, and a counter that interrupts the processor.
 *
 * Stage 13 gave the processor more program than it can see at once: a
 * cartridge that swaps 8 KB banks underneath it. That is a memory trick. The
 * chip in this stage does that too, and then it takes an interest in the
 * picture while the picture is being drawn:
 *
 *   the split        registers that say "these columns on the right of the
 *                    screen show a different nametable, scrolled to a
 *                    different place, drawn from a different pattern bank".
 *                    Half the screen stops obeying the scroll the other half
 *                    obeys, and nothing has to be redrawn to make that happen.
 *   the palettes     1 KB of the chip's own memory, one byte for every tile of
 *                    the background, saying which palette and which pattern
 *                    bank that single tile uses. A line of 32 tiles can
 *                    therefore be drawn in four different palettes, tile by
 *                    tile, while the line is being drawn.
 *   the counter      it counts the lines of the picture as they are drawn, and
 *                    when the count reaches the number the program chose, it
 *                    pulls the processor's interrupt wire. That is how a
 *                    program changes the palette, the scroll or anything else
 *                    half way down a frame instead of only between frames.
 *
 * This is a program for your computer, not for the board. The thing being
 * built is a chip and the way to see whether a chip works is to run a program
 * on it and look at the picture. Build it with
 *
 *     cc -Wall -Wextra main.c cpu.c ppu.c mmc5.c -o etap14 -lz
 *
 * and run it: it plays a small game for forty frames, writes the last frame to
 * etap14.png, and checks the three ideas above against what the machine did.
 *
 * The game itself is a cartridge image in this file: the processor's program
 * is 181 bytes of 6502 code, written out below one instruction per line, and
 * the level is a wall of tiles with windows and torches in it. The chip is
 * what this stage is about; the game exists to drive it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>       /* only for the picture file; see write_png below */

#include "cpu.h"
#include "mmc5.h"
#include "ppu.h"

/* ------------------------------------------------------------------ colours
 * The console's colours are numbers: three bits of red, three of green and two
 * of blue, packed into one byte. Storing a number and looking up the colour
 * when the pixel is drawn is what makes a whole screen fit in memory. It pays
 * to read these as three numbers rather than as a byte: 0x6E is "three of red,
 * three of green, two of blue", which is a grey.
 */
#define C_DARK    0x26      /* behind everything: a near-black blue  */
#define C_NIGHT   0x07      /* the sky seen through the windows      */
#define C_STAR    0xB6      /* pale light                            */
#define C_STONE   0x6E      /* the castle's grey                     */
#define C_STONE2  0x92      /* the lit edge of a stone               */
#define C_TORCH   0xB2      /* what the stone becomes below the line */
#define C_FIRE    0xC8      /* a torch's flame, low                  */
#define C_FLAME   0xF9      /* a torch's flame, high                 */
#define C_TEXT    0xFF      /* the panel's letters                   */

/* The four palettes of the background and the first palette of the sprites, in
 * the order the picture chip keeps them. This table is part of the cartridge:
 * the game copies it into the picture chip during its first frame, from the
 * banked window it has just pointed at this data. */
static const uint8_t cartridge_palette[32] = {
    /* $3F00  palette 0: the night sky in the windows */
    C_DARK,  C_NIGHT, C_STAR,  C_DARK,
    /* $3F04  palette 1: the castle's stone. The game's interrupt handler
     * rewrites $3F05 half way down every frame, and that single byte is what
     * lights the bottom of the picture. */
    C_DARK,  C_STONE, C_STONE2, C_DARK,
    /* $3F08  palette 2: the torches */
    C_DARK,  C_FIRE,  C_FLAME, C_DARK,
    /* $3F0C  palette 3: the panel at the side of the screen */
    C_DARK,  C_TEXT,  C_STAR,  C_DARK,
    /* $3F10  sprite palette 0: the one the hero uses */
    C_DARK,  C_STONE2, C_TEXT, C_DARK,
    /* $3F14  the three sprite palettes nothing in this cartridge uses */
    C_DARK,  C_DARK,  C_DARK,  C_DARK,
    C_DARK,  C_DARK,  C_DARK,  C_DARK,
    C_DARK,  C_DARK,  C_DARK,  C_DARK,
};

/* ------------------------------------------------------------- the program
 * The game, assembled by hand into the bank the chip wires to the top of the
 * map. Each line is one instruction: the address it lands on, the bytes, and
 * the instruction itself, so the whole program can be read here. The vectors
 * at the end of the address space are filled in at the bottom of the table.
 *
 * Two zero-page bytes are its variables: $00 is the world's scroll, one pixel
 * per frame, and $01 counts frames so that the hero's step changes every eight.
 */
#define GAME_BANK   3               /* the last bank, wired to $E000 */
#define GAME_STUB_ACK 0xE0B0        /* a handler that only acknowledges   */
#define GAME_STUB_RTI 0xE0B4        /* a handler that does nothing at all */

static const uint8_t game_code[] = {
    /* $E000  sei                    */ [0x0000] = 0x78,
    /* $E001  cld                    */ [0x0001] = 0xD8,
    /* $E002  ldx #$FF               */ [0x0002] = 0xA2, 0xFF,
    /* $E004  txs                    */ [0x0004] = 0x9A,
    /* ---- the cartridge chip: what the picture should be, and when the
     * counter should pull the processor's interrupt wire ---------------- */
    /* $E005  lda #$01               */ [0x0005] = 0xA9, 0x01,
    /* $E007  sta $5104              */ [0x0007] = 0x8D, 0x04, 0x51,
    /* $E00A  lda #$01               */ [0x000A] = 0xA9, 0x01,
    /* $E00C  sta $5117              */ [0x000C] = 0x8D, 0x17, 0x51,
    /* $E00F  lda #$DA               */ [0x000F] = 0xA9, 0xDA,
    /* $E011  sta $5200              */ [0x0011] = 0x8D, 0x00, 0x52,
    /* $E014  lda #$00               */ [0x0014] = 0xA9, 0x00,
    /* $E016  sta $5201              */ [0x0016] = 0x8D, 0x01, 0x52,
    /* $E019  lda #$02               */ [0x0019] = 0xA9, 0x02,
    /* $E01B  sta $5202              */ [0x001B] = 0x8D, 0x02, 0x52,
    /* $E01E  lda #$03               */ [0x001E] = 0xA9, 0x03,
    /* $E020  sta $5120              */ [0x0020] = 0x8D, 0x20, 0x51,
    /* $E023  lda #200               */ [0x0023] = 0xA9, 0xC8,
    /* $E025  sta $5203              */ [0x0025] = 0x8D, 0x03, 0x52,
    /* $E028  lda #$80               */ [0x0028] = 0xA9, 0x80,
    /* $E02A  sta $5204              */ [0x002A] = 0x8D, 0x04, 0x52,
    /* ---- the picture chip: both halves of the picture on, and the hero
     * placed four bytes into the sprite list ---------------------------- */
    /* $E02D  lda #$1E               */ [0x002D] = 0xA9, 0x1E,
    /* $E02F  sta $2001              */ [0x002F] = 0x8D, 0x01, 0x20,
    /* $E032  lda #$00               */ [0x0032] = 0xA9, 0x00,
    /* $E034  sta $2003              */ [0x0034] = 0x8D, 0x03, 0x20,
    /* $E037  lda #120               */ [0x0037] = 0xA9, 0x78,
    /* $E039  sta $2004              */ [0x0039] = 0x8D, 0x04, 0x20,
    /* $E03C  lda #$00               */ [0x003C] = 0xA9, 0x00,
    /* $E03E  sta $2004              */ [0x003E] = 0x8D, 0x04, 0x20,
    /* $E041  lda #$00               */ [0x0041] = 0xA9, 0x00,
    /* $E043  sta $2004              */ [0x0043] = 0x8D, 0x04, 0x20,
    /* $E046  lda #100               */ [0x0046] = 0xA9, 0x64,
    /* $E048  sta $2004              */ [0x0048] = 0x8D, 0x04, 0x20,
    /* ---- the palette, copied out of the second bank one byte at a time,
     * because that is where the cartridge keeps it ---------------------- */
    /* $E04B  lda #$3F               */ [0x004B] = 0xA9, 0x3F,
    /* $E04D  sta $2006              */ [0x004D] = 0x8D, 0x06, 0x20,
    /* $E050  lda #$00               */ [0x0050] = 0xA9, 0x00,
    /* $E052  sta $2006              */ [0x0052] = 0x8D, 0x06, 0x20,
    /* $E055  ldx #$00               */ [0x0055] = 0xA2, 0x00,
    /* $E057  pal: lda $A000,X       */ [0x0057] = 0xBD, 0x00, 0xA0,
    /* $E05A  sta $2007              */ [0x005A] = 0x8D, 0x07, 0x20,
    /* $E05D  inx                    */ [0x005D] = 0xE8,
    /* $E05E  cpx #32                */ [0x005E] = 0xE0, 0x20,
    /* $E060  bne pal                */ [0x0060] = 0xD0, 0xF5,
    /* $E062  cli                    */ [0x0062] = 0x58,
    /* ---- one pass per frame: wait for the picture to be over, then do the
     * work that must not be seen half done ------------------------------ */
    /* $E063  frame: lda $2002       */ [0x0063] = 0xAD, 0x02, 0x20,
    /* $E066  wait: lda $2002        */ [0x0066] = 0xAD, 0x02, 0x20,
    /* $E069  bpl wait               */ [0x0069] = 0x10, 0xFB,
    /* $E06B  lda #$3F               */ [0x006B] = 0xA9, 0x3F,
    /* $E06D  sta $2006              */ [0x006D] = 0x8D, 0x06, 0x20,
    /* $E070  lda #$05               */ [0x0070] = 0xA9, 0x05,
    /* $E072  sta $2006              */ [0x0072] = 0x8D, 0x06, 0x20,
    /* $E075  lda $A005              */ [0x0075] = 0xAD, 0x05, 0xA0,
    /* $E078  sta $2007              */ [0x0078] = 0x8D, 0x07, 0x20,
    /* $E07B  lda #$00               */ [0x007B] = 0xA9, 0x00,
    /* $E07D  sta $2005              */ [0x007D] = 0x8D, 0x05, 0x20,
    /* $E080  inc $00                */ [0x0080] = 0xE6, 0x00,
    /* $E082  lda $00                */ [0x0082] = 0xAD, 0x00, 0x00,
    /* $E085  sta $2005              */ [0x0085] = 0x8D, 0x05, 0x20,
    /* $E088  lda #$01               */ [0x0088] = 0xA9, 0x01,
    /* $E08A  sta $2003              */ [0x008A] = 0x8D, 0x03, 0x20,
    /* $E08D  inc $01                */ [0x008D] = 0xE6, 0x01,
    /* $E08F  lda $01                */ [0x008F] = 0xAD, 0x01, 0x00,
    /* $E092  lsr acc                */ [0x0092] = 0x4A,
    /* $E093  lsr acc                */ [0x0093] = 0x4A,
    /* $E094  lsr acc                */ [0x0094] = 0x4A,
    /* $E095  and #$01               */ [0x0095] = 0x29, 0x01,
    /* $E097  sta $2004              */ [0x0097] = 0x8D, 0x04, 0x20,
    /* $E09A  jmp frame              */ [0x009A] = 0x4C, 0x63, 0xE0,
    /* ---- the interrupt: acknowledge the counter, then repaint one entry of
     * the palette. Everything drawn from here to the bottom of the frame uses
     * the colour this writes ------------------------------------------- */
    /* $E09D  irq: lda $5204         */ [0x009D] = 0xAD, 0x04, 0x52,
    /* $E0A0  lda #$3F               */ [0x00A0] = 0xA9, 0x3F,
    /* $E0A2  sta $2006              */ [0x00A2] = 0x8D, 0x06, 0x20,
    /* $E0A5  lda #$05               */ [0x00A5] = 0xA9, 0x05,
    /* $E0A7  sta $2006              */ [0x00A7] = 0x8D, 0x06, 0x20,
    /* $E0AA  lda #$B2               */ [0x00AA] = 0xA9, 0xB2,
    /* $E0AC  sta $2007              */ [0x00AC] = 0x8D, 0x07, 0x20,
    /* $E0AF  rti                    */ [0x00AF] = 0x40,
    /* ---- two stubs, not used by the game: the checks below point the
     * interrupt vector at them to see what a handler does and does not do -- */
    /* $E0B0  stub_ack: lda $5204    */ [0x00B0] = 0xAD, 0x04, 0x52,
    /* $E0B3  rti                    */ [0x00B3] = 0x40,
    /* $E0B4  stub_rti: rti          */ [0x00B4] = 0x40,
    /* $FFFA  vector: stub_rti         */ [0x1FFA] = 0xB4, [0x1FFB] = 0xE0,
    /* $FFFC  vector: reset            */ [0x1FFC] = 0x00, [0x1FFD] = 0xE0,
    /* $FFFE  vector: irq              */ [0x1FFE] = 0x9D, [0x1FFF] = 0xE0,
};

/* -------------------------------------------------------------- the level
 * The world is one screen of tiles: a castle wall two bricks thick, with
 * pillars every eight columns, windows into the night, and torches. The chip
 * holds the map (which tile) and its own memory holds one byte per tile (which
 * palette, which pattern bank), so the wall, the torch and the window on one
 * line come out of three different palettes.
 */
#define MAP_W 32
#define MAP_H 30

/* Bank 0: the wall itself. */
#define TILE_BRICK   0
#define TILE_BRICK2  1
#define TILE_WINDOW  2
/* Bank 1: what is set into the wall. */
#define TILE_PILLAR  0
#define TILE_TORCH   1
/* Bank 2 holds one tile per character; 36 of them are the letters and digits
 * and the last one is a blank. */
#define TILE_SPACE   36
/* Bank 3 holds the hero. */
#define TILE_HERO    0

#define PAL_NIGHT  0
#define PAL_STONE  1
#define PAL_FIRE   2
#define PAL_PANEL  3

#define PANEL_COL  26           /* the first column the split covers */
#define PANEL_BANK 2
#define HERO_BANK  3

static const char art_brick[65] =
    "11111110"
    "11111112"
    "11111112"
    "11111112"
    "00000000"
    "21111111"
    "21111111"
    "21111111";

static const char art_brick2[65] =
    "11101111"
    "11101111"
    "11101111"
    "00000000"
    "11111011"
    "11111011"
    "11111011"
    "00000000";

static const char art_window[65] =
    "22222222"
    "21111112"
    "21122112"
    "21111112"
    "21112112"
    "21111112"
    "21111112"
    "22222222";

static const char art_pillar[65] =
    "11222211"
    "11333311"
    "11333311"
    "11333311"
    "11333311"
    "11333311"
    "11333311"
    "11222211";

static const char art_torch[65] =
    "00000000"
    "00012000"
    "00122100"
    "00122100"
    "00011000"
    "00033000"
    "00033000"
    "00033000";

static const char art_hero[65] =
    "00333300"
    "03111130"
    "03133130"
    "03111130"
    "00222200"
    "01111110"
    "01111110"
    "03000030";

static const char art_hero_step[65] =
    "00333300"
    "03111130"
    "03133130"
    "03111130"
    "00222200"
    "01111110"
    "01111110"
    "03003000";

/* Where the windows and the torches are. Nothing sits in the top five rows:
 * the picture wraps around at 240 pixels, and a feature there would jump when
 * it does. The panel covers the last six columns, so nothing sits there
 * either. */
struct spot {
    int row, col;
};

static const struct spot windows[] = {
    { 6, 5 }, { 11, 13 }, { 16, 21 }, { 21, 9 }, { 26, 17 }, { 8, 29 },
};
static const struct spot torches[] = {
    { 5, 10 }, { 10, 18 }, { 15, 2 }, { 20, 24 }, { 25, 14 }, { 28, 6 },
};

static int at_any(const struct spot *list, int count, int row, int col)
{
    for (int i = 0; i < count; i++) {
        if (list[i].row == row && list[i].col == col) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- the art
 * A tile is eight rows of eight pixels, each pixel a colour number 0 to 3. A
 * pattern stored the way the console stores it is two bytes per row: one bit
 * per pixel in the first byte, one in the second. This is that conversion, and
 * the only place in this stage where the two arrangements meet.
 */
static void put_tile(int bank, int index, const char *art)
{
    for (int row = 0; row < 8; row++) {
        uint8_t low = 0, high = 0;
        for (int x = 0; x < 8; x++) {
            int pixel = art[row * 8 + x] - '0';
            if (pixel & 1) low = (uint8_t)(low | (0x80 >> x));
            if (pixel & 2) high = (uint8_t)(high | (0x80 >> x));
        }
        mmc5_load_chr((uint8_t)bank, (uint16_t)(index * 16 + row * 2), low);
        mmc5_load_chr((uint8_t)bank, (uint16_t)(index * 16 + row * 2 + 1), high);
    }
}

/* The letters and digits of the panel, five pixels tall and three wide, drawn
 * twice as wide as they are stored so that six of them fill the panel. */
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

static void put_glyph(int bank, int index, const uint8_t *glyph)
{
    char art[65];
    for (int i = 0; i < 64; i++) art[i] = '0';
    art[64] = '\0';
    for (int row = 0; row < 5; row++) {
        for (int c = 0; c < 3; c++) {
            if (glyph[row] & (1u << (2 - c))) {
                art[(row + 1) * 8 + c * 2] = '1';
                art[(row + 1) * 8 + c * 2 + 1] = '1';
            }
        }
    }
    put_tile(bank, index, art);
}

static void chr_build(void)
{
    put_tile(0, TILE_BRICK, art_brick);
    put_tile(0, TILE_BRICK2, art_brick2);
    put_tile(0, TILE_WINDOW, art_window);
    put_tile(1, TILE_PILLAR, art_pillar);
    put_tile(1, TILE_TORCH, art_torch);
    put_tile(HERO_BANK, TILE_HERO, art_hero);
    put_tile(HERO_BANK, TILE_HERO + 1, art_hero_step);

    for (int i = 0; glyph_names[i]; i++) put_glyph(PANEL_BANK, i, glyphs[i]);

    /* One tile more, with no pixels in it: the space between two words of the
     * panel is a tile like any other. */
    {
        char blank[65];
        for (int i = 0; i < 64; i++) blank[i] = '0';
        blank[64] = '\0';
        put_tile(PANEL_BANK, TILE_SPACE, blank);
    }
}

static void panel_text(int row, int col, const char *text)
{
    for (int i = 0; text[i]; i++) {
        const char *at = strchr(glyph_names, text[i]);
        int tile = at ? (int)(at - glyph_names) : TILE_SPACE;
        mmc5_load_nt(1, row * MAP_W + col + i, (uint8_t)tile);
    }
}

static void level_build(void)
{
    /* The wall, its pillars, its windows and its torches, with one byte of the
     * chip's own memory per tile saying which palette and which bank that tile
     * draws with. */
    for (int row = 0; row < MAP_H; row++) {
        for (int col = 0; col < MAP_W; col++) {
            int tile = TILE_BRICK + (row & 1);
            int bank = 0;
            int palette = PAL_STONE;

            if (col % 8 == 3) {
                tile = TILE_PILLAR;
                bank = 1;
            }
            if (at_any(windows, (int)(sizeof windows / sizeof windows[0]),
                       row, col)) {
                tile = TILE_WINDOW;
                palette = PAL_NIGHT;
            }
            if (at_any(torches, (int)(sizeof torches / sizeof torches[0]),
                       row, col)) {
                tile = TILE_TORCH;
                bank = 1;
                palette = PAL_FIRE;
            }

            mmc5_load_nt(0, row * MAP_W + col, (uint8_t)tile);
            mmc5_load_attr(row * MAP_W + col,
                           (uint8_t)((palette << 6) | bank));
        }
    }

    /* The panel is the second nametable: its own map, its own palette, its own
     * pattern bank. Nothing in it moves, which is the point of the split. */
    for (int i = 0; i < MAP_W * MAP_H; i++) mmc5_load_nt(1, i, TILE_SPACE);
    for (int i = 0x3C0; i < 0x400; i++) mmc5_load_nt(1, i, 0xFF);
    panel_text(1, PANEL_COL, "CASTLE");
    panel_text(3, PANEL_COL, "SCORE");
    panel_text(4, PANEL_COL, "001250");
    panel_text(6, PANEL_COL, "LIVES");
    panel_text(7, PANEL_COL + 4, "3");
    panel_text(9, PANEL_COL, "FLOOR");
    panel_text(10, PANEL_COL + 4, "2");
    panel_text(13, PANEL_COL, "MMC5");
    panel_text(16, PANEL_COL, "SPLIT");
    panel_text(17, PANEL_COL + 4, "ON");
}

/* ---------------------------------------------------------------- the machine
 * The processor, the picture chip and the cartridge chip, and one address
 * space that all three of them answer for. Nothing here decides anything: it
 * hands the address to whoever owns it, which is what stage 06 called a bus.
 */
#define PRG_BANKS 4
#define PRG_SIZE  (PRG_BANKS * 0x2000)

/* How many processor instructions fit between two lines of the picture. A real
 * console gives the processor about 113 ticks per line and an instruction
 * takes three or four of them, so this is that budget, rounded. It is what
 * makes the game's timing real: it waits for the frame flag during vblank
 * because that is when the flag is up, not because the program says so. */
#define INSTRUCTIONS_PER_LINE 40
#define VBLANK_LINES          60
#define DEMO_FRAMES           40

static uint8_t cartridge[PRG_SIZE];
static uint8_t ram[0x800];

static struct {
    int row;            /* the line being drawn right now               */
    int frame;
    int irq_count;      /* interrupts taken during the frame being run  */
    int irq_line;       /* the first line the handler's work applies to */
} machine;

uint8_t machine_read(uint16_t addr)
{
    if (addr < 0x2000) return ram[addr & 0x7FF];
    if (addr < 0x2008) return ppu_read(addr);
    if (addr >= 0x5100 && addr <= 0x5FFF) return mmc5_read(addr);
    if (addr >= 0x8000) return mmc5_prg_read(addr);
    return 0;
}

void machine_write(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) { ram[addr & 0x7FF] = value; return; }
    if (addr < 0x2008) { ppu_write(addr, value); return; }
    if (addr >= 0x5100 && addr <= 0x5FFF) { mmc5_write(addr, value); return; }
    /* There is nothing else: the program memory is a ROM and ignores writes. */
}

int machine_irq(void)
{
    return mmc5_irq();
}

void machine_irq_taken(void)
{
    machine.irq_count++;
    /* The counter has just finished line `row`, so the handler's writes are in
     * time for the line after it. */
    if (machine.irq_count == 1) machine.irq_line = machine.row + 1;
}

static void cartridge_build(void)
{
    for (int i = 0; i < 32; i++) cartridge[0x2000 + i] = cartridge_palette[i];
    for (int i = 0; i < (int)sizeof game_code; i++) {
        cartridge[GAME_BANK * 0x2000 + i] = game_code[i];
    }
}

/* Point the interrupt vector somewhere else, the way the checks below use a
 * handler that does less than the game's. */
static void set_irq_vector(uint16_t addr)
{
    cartridge[GAME_BANK * 0x2000 + 0x1FFE] = (uint8_t)(addr & 0xFF);
    cartridge[GAME_BANK * 0x2000 + 0x1FFF] = (uint8_t)(addr >> 8);
}

static void machine_reset(void)
{
    for (int i = 0; i < (int)sizeof ram; i++) ram[i] = 0;
    machine.row = 0;
    machine.frame = 0;
    machine.irq_count = 0;
    machine.irq_line = 0;

    ppu_reset();
    mmc5_reset();
    cartridge_build();
    mmc5_set_cartridge(cartridge, PRG_BANKS);
    chr_build();
    level_build();

    /* Every sprite starts below the bottom of the picture, so the 63 the game
     * never places do not appear in a corner of it. */
    ppu_write(0x2003, 0);
    for (int i = 0; i < 256; i++) ppu_write(0x2004, 0xF0);

    cpu_reset();                /* reads the reset vector out of the cartridge */
}

static void machine_run_frame(void)
{
    machine.irq_count = 0;

    mmc5_set_drawing(1);
    for (int row = 0; row < PPU_H; row++) {
        machine.row = row;
        ppu_render_row(row);
        mmc5_tick_line(row);        /* the counter, and possibly the interrupt */
        cpu_run(INSTRUCTIONS_PER_LINE);
    }
    mmc5_set_drawing(0);

    /* The picture is over. The processor gets the rest of the frame for its
     * own work. This is vblank, and it is where the game reads the flag that
     * says a new frame has begun and does everything that must not be seen
     * half done. */
    machine.row = PPU_H - 1;
    ppu_set_vblank(1);
    cpu_run(INSTRUCTIONS_PER_LINE * VBLANK_LINES);
    ppu_set_vblank(0);              /* the flag falls if nobody read it */

    machine.frame++;
}

/* ------------------------------------------------------------ picture file
 * A PNG file is a signature, then a few named blocks, each with the length of
 * its contents and a check value over them, and inside one block the pixels.
 * The alternative was a file of raw bytes, which no viewer opens. None of the
 * console ideas live in this part.
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
    if (!buffer) return;
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

/* One colour number as the three bytes a picture file wants. The blue channel
 * has one bit less than the others, so it is stretched to the same range;
 * without that, "all bits set" would come out slightly yellow. */
static void palette_rgb(int index, unsigned char *rgb)
{
    uint8_t c = (uint8_t)index;
    rgb[0] = (unsigned char)((c >> 5) & 7);
    rgb[1] = (unsigned char)((c >> 2) & 7);
    rgb[2] = (unsigned char)(c & 3);
    for (int i = 0; i < 3; i++) {
        int max_in = (i == 2) ? 3 : 7;
        rgb[i] = (unsigned char)((rgb[i] * 255) / max_in);
    }
}

static int write_png(const char *path)
{
    static const unsigned char signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char header[13];
    unsigned char *raw, *packed;
    uLongf packed_len;
    size_t stride = (size_t)PPU_W * 3 + 1;
    size_t raw_len = (size_t)PPU_H * stride;
    FILE *f;

    raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < PPU_H; y++) {
        unsigned char *at = raw + (size_t)y * stride;
        *at++ = 0;                          /* the row's filter: none */
        for (int x = 0; x < PPU_W; x++) {
            palette_rgb(ppu_framebuffer[y][x], at);
            at += 3;
        }
    }
    packed_len = compressBound((uLong)raw_len);
    packed = malloc(packed_len);
    if (!packed) { free(raw); return 0; }
    if (compress2(packed, &packed_len, raw, (uLong)raw_len, 9) != Z_OK) {
        free(packed);
        free(raw);
        return 0;
    }
    free(raw);

    f = fopen(path, "wb");
    if (!f) { free(packed); return 0; }
    fwrite(signature, 1, 8, f);
    be32(header, PPU_W);
    be32(header + 4, PPU_H);
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

/* ------------------------------------------------------------------ checks */

static int failures;

static void expect(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int row_has(int row, uint8_t colour)
{
    for (int x = 0; x < PPU_W; x++) {
        if (ppu_framebuffer[row][x] == colour) return 1;
    }
    return 0;
}

static int any_row_has(uint8_t colour)
{
    for (int y = 0; y < PPU_H; y++) {
        if (row_has(y, colour)) return 1;
    }
    return 0;
}

/* The world climbs by one pixel per frame, so what the last frame showed one
 * line further down is what this frame shows here: frame[y] is frame-1[y + 1].
 * The comparison stays above the sprite and away from the left and right
 * edges, where the picture bends round. */
static int world_moved_one_line(const uint8_t before[PPU_H][PPU_W])
{
    for (int y = 40; y < 110; y++) {
        for (int x = 8; x < 200; x++) {
            if (ppu_framebuffer[y][x] != before[y + 1][x]) return 0;
        }
    }
    return 1;
}

static int panel_is_unchanged(const uint8_t before[PPU_H][PPU_W])
{
    for (int y = 0; y < PPU_H; y++) {
        for (int x = PANEL_COL * 8; x < PPU_W; x++) {
            if (ppu_framebuffer[y][x] != before[y][x]) return 0;
        }
    }
    return 1;
}

static void check_the_counter(void)
{
    /* The counter on its own, without a processor: it should ask for an
     * interrupt once, after the line the program named, and never for a target
     * of zero. The flag is a latch that stays up until somebody reads it, so
     * this loop acknowledges it the way a handler does — otherwise the second
     * half of the frame would look like forty more interrupts. */
    int asked_at = -1, asked_twice = 0;

    mmc5_reset();
    mmc5_write(0x5203, 200);
    mmc5_write(0x5204, 0x80);
    for (int row = 0; row < PPU_H; row++) {
        mmc5_tick_line(row);
        if (mmc5_irq()) {
            if (asked_at < 0) asked_at = row + 1;
            else              asked_twice++;
            (void)mmc5_read(0x5204);    /* acknowledge it */
        }
    }
    expect(asked_at == 200 && asked_twice == 0,
           "the counter asks once, after 200 lines");

    mmc5_reset();
    mmc5_write(0x5204, 0x80);           /* enabled, but the target is zero */
    int asked = 0;
    for (int row = 0; row < PPU_H; row++) {
        mmc5_tick_line(row);
        if (mmc5_irq()) asked = 1;
    }
    expect(!asked, "a target of zero never asks");
}

static void check_a_run(int frames)
{
    for (int i = 0; i < frames; i++) machine_run_frame();
}

int main(int argc, char **argv)
{
    static uint8_t before[PPU_H][PPU_W];
    const char *path = "etap14.png";

    if (argc > 1) path = argv[1];

    crc_init();
    printf("stage 14: the cartridge chip, the split, and the line counter\n\n");

    /* ---- the chip's counter, on its own */
    check_the_counter();

    /* ---- the game, played */
    machine_reset();
    check_a_run(DEMO_FRAMES);

    printf("frame %d, the world scrolled %u pixels, interrupts in it: %d"
           " (line %d)\n",
           machine.frame, (unsigned)ram[0x00], machine.irq_count,
           machine.irq_line);
    printf("the panel is columns %d..%d of the picture\n",
           PANEL_COL * 8, PPU_W - 1);

    if (!write_png(path)) {
        printf("could not write %s\n", path);
        return 1;
    }
    printf("picture written to %s\n\n", path);

    expect(machine.irq_count == 1 && machine.irq_line == 200,
           "one interrupt per frame, arriving between lines 199 and 200");
    expect(ram[0x00] == DEMO_FRAMES && !cpu.stopped,
           "the processor ran the game's own loop, once per frame");

    /* The handler rewrites one palette entry half way down the frame: the same
     * stone is grey above the line and lit by torches below it. */
    {
        int none_above = 1, some_below = 0;
        for (int y = 0; y < 200; y++) {
            if (row_has(y, C_TORCH)) none_above = 0;
        }
        for (int y = 200; y < PPU_H; y++) {
            if (row_has(y, C_TORCH)) some_below = 1;
        }
        expect(row_has(100, C_STONE) && none_above,
               "above the line the wall is the colour the cartridge holds");
        expect(some_below,
               "below it the wall is the colour the handler wrote");
    }

    /* ---- the same frame with a handler that only acknowledges */
    machine_reset();
    set_irq_vector(GAME_STUB_ACK);
    check_a_run(DEMO_FRAMES);
    expect(machine.irq_count == 1,
           "the interrupt still arrives when the handler does less");
    expect(!any_row_has(C_TORCH) && row_has(220, C_STONE),
           "an interrupt that changes nothing changes nothing");

    /* The flag is a latch, and reading the register is the only thing that
     * clears it. A handler that does not read it is called again the moment it
     * returns, and the processor never gets back to the game. */
    machine_reset();
    set_irq_vector(GAME_STUB_RTI);
    check_a_run(3);
    expect(machine.irq_count > 100,
           "an interrupt nobody acknowledges asks again and again");

    /* ---- the split: half the picture obeys the scroll, half of it does not */
    machine_reset();
    check_a_run(DEMO_FRAMES);
    memcpy(before, ppu_framebuffer, sizeof before);
    machine_run_frame();
    expect(world_moved_one_line(before),
           "the world is the frame before it, one line further on");
    expect(panel_is_unchanged(before),
           "the panel has not moved while the world did");

    /* The split has a scroll of its own, and moving it moves the panel and
     * nothing else. That is the whole point of the register. */
    machine_reset();
    check_a_run(DEMO_FRAMES);
    memcpy(before, ppu_framebuffer, sizeof before);
    mmc5_write(0x5201, 8);              /* the panel's own picture, one tile down */
    machine_run_frame();
    expect(world_moved_one_line(before) && !panel_is_unchanged(before),
           "the split's own scroll moves the panel and leaves the world");

    /* ---- the palettes: one line, three palettes, tile by tile */
    machine_reset();
    check_a_run(DEMO_FRAMES);
    {
        int line_with_two = 0;
        for (int y = 0; y < PPU_H && !line_with_two; y++) {
            if (row_has(y, C_STONE) && row_has(y, C_FIRE)) line_with_two = 1;
        }
        expect(line_with_two, "one line of the wall carries two palettes");
        expect(any_row_has(C_NIGHT), "and a third one is in the windows");
        /* The panel's rows are numbered from the top of its own nametable,
         * so row 4 of the map is drawn 32 lines down the screen. */
        expect(row_has(4 * 8 + 4, C_TEXT),
               "the panel draws with a palette of its own");
    }

    /* With the chip's own memory switched off, the picture falls back to the
     * one attribute byte per 4 by 4 tiles that stage 08 knew about. */
    machine_reset();
    check_a_run(10);
    mmc5_write(0x5104, 0);              /* the chip's memory is out of the way */
    check_a_run(2);
    expect(!any_row_has(C_FIRE) && !any_row_has(C_STONE),
           "without the chip's memory the per-tile palettes are gone");
    expect(row_has(4 * 8 + 4, C_TEXT),
           "the panel keeps its own palette either way");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
