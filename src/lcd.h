/*
 * lcd.h — NES display output.
 *
 * The panel runs in landscape: a 320x240 window with the 256x240 NES
 * picture centred (32-pixel black bars on the sides, painted once at
 * boot). The framebuffer holds only the NES picture, one byte per pixel,
 * using the 64 NES colours as palette indices.
 */
#pragma once
#include <stdint.h>

#define LCD_W     320            /* the panel, landscape              */
#define LCD_H     240
#define NES_X     32             /* where the NES picture starts      */
#define NES_W     256            /* ... and how big it is             */
#define FB_W      NES_W          /* the framebuffer is the picture    */
#define FB_H      LCD_H
#define BAND_H    8              /* scanlines per SPI transfer        */
#define BAND_BYTES (NES_W * BAND_H * 2)

void lcd_init(void);

/* emulator line hook: copy a rendered scanline in, stream a band out */
void lcd_nes_line(int y, const uint8_t *line);
void lcd_nes_frame_end(void);

/* text overlay for boot status and the fps readout */
void lcd_clear(uint8_t color_index);
void lcd_text(int16_t x, int16_t y, const char *s, uint8_t fg, uint8_t bg);
void lcd_show_fps(int fps);
void lcd_push_full(void);   /* one-off CPU-driven push of the picture */
