/*
 * lcd.h — NES display output.
 *
 * The panel runs in landscape: a 320x240 window with the 256x240 NES
 * picture centred (32-pixel black bars on the sides, painted once at
 * boot). The framebuffer holds only the NES picture, one byte per pixel,
 * using the 64 NES colours as palette indices.
 *
 * Two pixel formats are supported, chosen at compile time:
 *
 *   default        RGB565, 16 bits/pixel (2 bytes), COLMOD 0x55
 *   -DLCD_12BIT    RGB444, 12 bits/pixel (2 pixels in 3 bytes), COLMOD 0x53
 *
 * 12-bit mode is 25% less traffic on the SPI wire (92,160 bytes a frame
 * instead of 122,880), which is the frame-time ceiling on this board; it
 * costs colour resolution, 4 bits per channel instead of 5/6/5. See
 * docs/PERFORMANCE.md for the numbers and lcd.c for the packing rule.
 */
#pragma once
#include <stdint.h>

#define LCD_W     320            /* the panel, landscape              */
#define LCD_H     240
#define NES_X     32             /* where the NES picture starts      */
#define NES_W     256            /* ... and how big it is             */
#define FB_W      NES_W          /* the framebuffer is the picture    */
#define FB_H      LCD_H

#ifdef LCD_12BIT
#define LCD_BPP   12
/* the ST7789 packs two 12-bit pixels into three bytes */
#define BAND_BYTES (NES_W * BAND_H * 3 / 2)
#else
#define LCD_BPP   16
#define BAND_BYTES (NES_W * BAND_H * 2)
#endif

/* Scanlines per SPI transfer. Four, not eight, because the band staging is
 * double-buffered: the next band is converted while the previous one is
 * still on the wire, and two 4-line buffers (4 KB in 16-bit mode) cost the
 * same RAM as the single 8-line buffer they replace. A band of four lines
 * is 2 KB / 1.5 KB, i.e. 409 / 307 us on the wire, which is shorter than
 * the ~470 us of emulation that produces it — so the DMA is never the
 * thing the CPU waits for. */
#define BAND_H    4

void lcd_init(void);

/* The PPU renders into this row for scanline y (installed as
 * nes_line_target), so the picture never has to be copied. */
uint8_t *lcd_nes_line_target(int y);

/* emulator line hook: note whether the band changed, stream it out */
void lcd_nes_line(int y, const uint8_t *line);
void lcd_nes_frame_end(void);

/* text overlay for boot status and the fps readout */
void lcd_clear(uint8_t color_index);
void lcd_text(int16_t x, int16_t y, const char *s, uint8_t fg, uint8_t bg);
void lcd_show_fps(int fps);
void lcd_push_full(void);   /* one-off CPU-driven push of the picture */
