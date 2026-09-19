/*
 * mmc5.h — the cartridge chip: the most complicated one this guide covers.
 *
 * Stage 13 explained why a cartridge switches banks: the processor sees 32 KB
 * of program at a time and a game is bigger than that. This chip does that
 * too, and then three things more, all of them about the picture:
 *
 *   its own memory    1 KB that belongs to the chip. In the mode this stage
 *                     uses, every tile of the background has one byte there
 *                     saying which palette it draws with and which pattern
 *                     bank it comes from. Two tiles side by side can therefore
 *                     use two palettes, which the plain picture chip cannot do.
 *   the split         a run of tile columns on one side of the screen shows a
 *                     different nametable, with its own vertical scroll and
 *                     its own pattern bank. One chip, one screen, two pictures
 *                     that move independently.
 *   the line counter  it counts the lines of the picture as they are drawn.
 *                     When the count reaches the number the program chose, the
 *                     chip pulls the processor's interrupt line. That is how a
 *                     program changes the picture half way down a frame,
 *                     instead of only between frames.
 *
 * The rules here are the ones the finished emulator implements in
 * src/mapper.c, cut down to what a reader needs in order to see the idea.
 */
#pragma once
#include <stdint.h>

/* ------------------------------------------------------------------ fetching
 * The picture chip does not become pixels by itself. For every tile of every
 * line it asks the cartridge "what goes here?", and the cartridge answers.
 * This is what the two of them say to each other, and it is why the chip can
 * put a different nametable on one side of the screen: the question carries
 * the position, so the answer can ignore it.
 */
typedef struct {
    int col;        /* the fetch counter along the line: 0 is the left edge */
    int row;        /* the picture line being drawn: 0 is the top          */
    int nt_col;     /* the nametable column the scroll points at           */
    int nt_row;     /* the nametable row the scroll points at              */
    int fine_y;     /* the pixel row inside that tile                      */
} bg_fetch_t;

typedef struct {
    uint8_t tile;     /* which tile of the bank                     */
    uint8_t palette;  /* which of the four palettes it draws with   */
    uint8_t bank;     /* which pattern bank it comes from           */
    uint8_t fine_y;   /* which row inside the tile                  */
} bg_tile_t;

/* ------------------------------------------------------------------- machine */

void mmc5_reset(void);

/* True while the picture is being drawn. The chip lets the processor into its
 * own memory only then, which is the rule that makes the interrupt handler the
 * right place to change that memory. */
void mmc5_set_drawing(int on);

/* One line has been drawn. The counter moves, and may pull the interrupt. */
void mmc5_tick_line(int row);

/* True while the chip is asking for an interrupt. */
int  mmc5_irq(void);

/* The processor's view: the chip's registers and its memory. */
uint8_t mmc5_read(uint16_t addr);
void    mmc5_write(uint16_t addr, uint8_t value);

/* The program memory, in 8 KB windows. $E000 is always the last bank, so the
 * reset and interrupt vectors never move. */
uint8_t mmc5_prg_read(uint16_t addr);

/* The video memory the chip has taken over, as the picture chip sees it. */
uint8_t mmc5_nt_read(uint16_t addr);
void    mmc5_nt_write(uint16_t addr, uint8_t value);

/* One byte of a pattern: 16 bytes per tile, two of them per pixel row. */
uint8_t mmc5_pattern_byte(uint8_t bank, uint8_t tile, uint8_t offset);

/* Which pattern bank the sprites draw from. */
uint8_t mmc5_sprite_bank(void);

/* Answer the picture chip's question for one tile of one line. */
void mmc5_background(const bg_fetch_t *f, bg_tile_t *out);

/* -------------------------------------------------------- setting the machine up
 * The level, the art and the program are already in the machine when the first
 * line is drawn: a real game uploads them through the picture chip, and this
 * stage spends its room on the chip instead. These three functions are that
 * loading door, and the program in the cartridge cannot reach it.
 */
void mmc5_load_nt(int page, int offset, uint8_t value);
void mmc5_load_attr(int offset, uint8_t value);
void mmc5_load_chr(uint8_t bank, uint16_t offset, uint8_t value);

/* Hand the chip the program memory it answers reads from. */
void mmc5_set_cartridge(const uint8_t *prg, int banks);
