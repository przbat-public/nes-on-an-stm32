/*
 * ppu.h — the picture chip, reduced to what this stage needs.
 *
 * Stage 08 explained the trick the whole display rests on: the picture is
 * not stored as pixels, it is stored as numbers arranged in a grid, and the
 * colours are looked up when the picture is drawn. This file keeps exactly
 * that much of the chip:
 *
 *   2 KB of video memory holding 30 rows of 32 tiles   (which tile)
 *   64 bytes of tile shapes, 8 rows of 8 pixels each   (which pixels)
 *   32 bytes of palette                               (which colours)
 *
 * The game fills the first two through two registers: one says where in
 * video memory the next write goes, the other delivers the byte. That is
 * also why the same 64 bytes of tile shapes serve both halves of the
 * picture: the shape is shared, the colour comes from the palette.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PPU_W 256
#define PPU_H 240

void ppu_reset(void);

/* the six registers the processor can reach, at $2000-$2007 */
uint8_t ppu_read(uint16_t addr);
void    ppu_write(uint16_t addr, uint8_t value);

/* A frame has been drawn: raise the flag the game's loop waits for. */
void ppu_start_frame(void);

/* Turn video memory into pixels: one byte per pixel, a palette number. */
void ppu_render_picture(uint8_t *pixels);
