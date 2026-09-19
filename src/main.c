/*
 * main.c — the NES emulator: init the hardware, load the cartridge that
 * is baked into flash, then run frames forever.
 *
 *   render 262 scanlines (CPU + PPU in lockstep)
 *   stream the picture out in bands while the next frame is rendered
 *   read the pad once per frame
 */
#include "hal.h"
#include "lcd.h"
#include "input.h"
#include "nes.h"
#include "ppu.h"
#include "cpu6502.h"

/* diagnostics readable over SWD while the emulator runs */
volatile uint32_t dbg_frames, dbg_lines, dbg_bands, dbg_dma_ok, dbg_fps;
void lcd_set_fps(int fps);
void lcd_set_fps(int fps) { dbg_fps = (uint32_t)fps; }

/* the cartridge image, embedded by tools/rom2c.py from build/test.nes */
extern const uint8_t nes_rom[];
extern const uint32_t nes_rom_len;

static int  fps_frames;
static uint32_t fps_last;
static int  fps_show;

static void show_boot_text(const char *l1, const char *l2)
{
    lcd_clear(15);                       /* black */
    lcd_text(8, 90, l1, 32, 15);         /* white on black */
    if (l2) lcd_text(8, 104, l2, 33, 15);
    lcd_push_full();
}

static const char *mapper_name(int m)
{
    switch (m) {
    case 0:  return "NROM";
    case 1:  return "MMC1";
    case 2:  return "UXROM";
    case 4:  return "MMC3";
    default: return "UNKNOWN MAPPER";
    }
}

int main(void)
{
    system_init();
    lcd_init();
    input_init();

    int rc = nes_load(nes_rom, nes_rom_len);
    if (rc != NES_OK) {
        show_boot_text("CART LOAD ERROR", rc == NES_ERR_MAPPER
                       ? "UNSUPPORTED MAPPER NROM MMC1 UxROM MMC3"
                       : "BAD INES IMAGE");
        for (;;) {}
    }

    show_boot_text("NES ON STM32L476RG", mapper_name(nes_mapper));

    nes_line_hook = lcd_nes_line;
    nes_line_target = lcd_nes_line_target;   /* render straight into fb */
    nes_reset();

    fps_last = cycles_now();
    fps_show = 180;                      /* show the fps for ~3 s */

    for (;;) {
        nes_set_buttons(input_pad());
        nes_run_frame();
        dbg_frames++;
        lcd_nes_frame_end();
        dbg_bands++;

        /* frame rate every 32 frames, from the cycle counter */
        if (++fps_frames >= 32) {
            uint32_t now = cycles_now();
            uint32_t cycles = now - fps_last;
            fps_last = now;
            int fps = (int)(80000000ull * 32ull / (cycles ? cycles : 1ull));
            fps_frames = 0;
            dbg_fps = (uint32_t)fps;
            if (fps_show > 0) {
                lcd_show_fps(fps);
                fps_show -= 32;
            } else {
                lcd_show_fps(fps);       /* keep it updated, off-screen */
            }
        }
    }
}
