/*
 * main.c — the NES emulator: bring up the hardware, load the cartridge that
 * is baked into flash, then run frames forever.
 *
 * The frame loop is the whole program:
 *
 *   read the pad once per frame      (a pad read is part of a frame, not an
 *                                     interrupt, so the game sees a stable
 *                                     controller state for the whole frame)
 *   run one frame (262 scanlines)    (CPU and PPU in lockstep, see nes.c)
 *   finish the picture               (the last band is still on the wire)
 *
 * Everything else here is diagnostics: counters that the debugger reads over
 * SWD while the emulator runs, and a frame-rate readout for the panel.
 */
#include "hal.h"
#include "lcd.h"
#include "input.h"
#include "nes.h"
#include "ppu.h"
#include "cpu6502.h"

/* diagnostics readable over SWD while the emulator runs */
volatile uint32_t dbg_frames, dbg_lines, dbg_bands, dbg_dma_ok, dbg_fps;

/* the cartridge, embedded by tools/rom2c.py from whatever ROM was built in */
extern const uint8_t nes_rom[];
extern const uint32_t nes_rom_len;

/* The panel shows the frame rate for the first few seconds, so it is visible
 * that the emulator is alive without a debugger attached. */
#define FPS_WINDOW_FRAMES  32        /* frames between measurements       */
#define FPS_SHOW_FRAMES    180       /* ~3 s of on-screen readout         */
#define CPU_HZ             80000000u /* the L476 runs at 80 MHz (hal.c)   */

/* boot screen colours, as NES palette indices (see the table in lcd.c) */
#define BOOT_BG            15        /* black                             */
#define BOOT_FG            32        /* white                             */
#define BOOT_FG_DIM        33        /* light blue                        */
#define BOOT_X             8
#define BOOT_Y1            90
#define BOOT_Y2            104

static int      fps_frames;
static uint32_t fps_last;
static int      fps_show;

static void show_boot_text(const char *l1, const char *l2)
{
    lcd_clear(BOOT_BG);
    lcd_text(BOOT_X, BOOT_Y1, l1, BOOT_FG, BOOT_BG);
    if (l2) lcd_text(BOOT_X, BOOT_Y2, l2, BOOT_FG_DIM, BOOT_BG);
    lcd_push_full();
}

/* The mapper number is what the cartridge header claims; the name is here so
 * that a wrong cartridge is obvious on the panel rather than only in a dump. */
static const char *mapper_name(int m)
{
    switch (m) {
    case 0:  return "NROM";
    case 1:  return "MMC1";
    case 2:  return "UXROM";
    case 4:  return "MMC3";
    case 5:  return "MMC5";
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
        /* Stop here with the reason on the panel: a cartridge that cannot be
         * mapped has nothing useful to run, and a blank screen would leave the
         * reader guessing whether the build, the flash or the ROM is at fault. */
        show_boot_text("CART LOAD ERROR", rc == NES_ERR_MAPPER
                       ? "UNSUPPORTED MAPPER NROM MMC1 UxROM MMC3 MMC5"
                       : "BAD INES IMAGE");
        for (;;) {}
    }

    show_boot_text("NES ON STM32L476RG", mapper_name(nes_mapper));

    nes_line_hook   = lcd_nes_line;
    nes_line_target = lcd_nes_line_target;   /* render straight into fb */
    nes_reset();

    fps_last = cycles_now();
    fps_show = FPS_SHOW_FRAMES;

    for (;;) {
        nes_set_buttons(input_pad());
        nes_run_frame();
        dbg_frames++;
        lcd_nes_frame_end();       /* wait for the last band to leave */
        dbg_bands++;

        if (++fps_frames >= FPS_WINDOW_FRAMES) {
            uint32_t now    = cycles_now();
            uint32_t cycles = now - fps_last;
            fps_last = now;
            fps_frames = 0;

            int fps = (int)((uint64_t)CPU_HZ * FPS_WINDOW_FRAMES
                            / (cycles ? cycles : 1));
            dbg_fps = (uint32_t)fps;
            lcd_show_fps(fps);
            if (fps_show > 0) fps_show -= FPS_WINDOW_FRAMES;
        }
    }
}
