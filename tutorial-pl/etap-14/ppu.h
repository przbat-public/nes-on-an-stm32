/*
 * ppu.h — the picture chip, as much of it as this stage needs.
 *
 * Stage 08 explained what this chip does with the memory it is given: it
 * turns a map of tile numbers into pixels, eight at a time, one line at a
 * time. Stage 12 added the two things a game needs in order to move the
 * picture: a scroll register, and a rule that a write made while the picture
 * is being drawn takes effect on the next line — the moment between two lines
 * is the only gap the program gets.
 *
 * What is new here is the shape of the code rather than the chip: this file
 * draws one line at a time and calls into the cartridge for every tile of
 * that line. The cartridge therefore sees the picture being built, and can
 * answer differently for the left half and the right half of the same line.
 * That hook is the whole reason the next chapter's chip can exist.
 */
#pragma once
#include <stdint.h>

#define PPU_W 256
#define PPU_H 240

/* The picture, one byte per pixel holding a colour number. */
extern uint8_t ppu_framebuffer[PPU_H][PPU_W];

void ppu_reset(void);

/* The eight registers the processor can reach, at $2000-$2007. */
uint8_t ppu_read(uint16_t addr);
void    ppu_write(uint16_t addr, uint8_t value);

/* Draw one line of the picture. Called once per line, in order. */
void ppu_render_row(int row);

/* The picture is over: raise the flag the game's loop waits for. */
void ppu_set_vblank(int on);
