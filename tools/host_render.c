/*
 * host_render.c — runs the emulator on the host and dumps frames.
 *
 * This is how the PPU was developed and verified: load a .nes file, run
 * N frames, write the video output as a raw index buffer (.raw, 256x240
 * bytes) which tools/raw2png.py turns into a PNG with the NES palette.
 *
 *   make host-rom ROM=build/test.nes FRAMES=60 OUT=build/frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/nes.h"
#include "../src/ppu.h"
#include "../src/cpu6502.h"

static uint8_t screen[PPU_W * PPU_H];

void line_hook(int y, const uint8_t *line)
{
    memcpy(screen + y * PPU_W, line, PPU_W);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "build/test.nes";
    int frames = (argc > 2) ? atoi(argv[2]) : 60;
    const char *out = (argc > 3) ? argv[3] : "build/frame";

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)len);
    if (fread(rom, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 1; }
    fclose(f);

    int rc = nes_load(rom, (uint32_t)len);
    if (rc != NES_OK) {
        printf("nes_load failed: %d (0=ok, -1=format, -2=mapper, -3=size)\n", rc);
        return 1;
    }
    printf("ROM: %s (%ld bytes, mapper %d, PRG %d x 16K, CHR %d x 8K)\n",
           path, len, nes_mapper, nes_prg_banks, nes_chr_banks);

    nes_line_hook = line_hook;
    nes_reset();

    /* button test: press START for a while (proves the pad path) */
    for (int i = 0; i < frames; i++) {
        nes_set_buttons((i > 5 && i < 20) ? PAD_START : 0);
        nes_run_frame();
    }

    char name[256];
    snprintf(name, sizeof(name), "%s.raw", out);
    FILE *o = fopen(name, "wb");
    if (!o) { perror(name); return 1; }
    fwrite(screen, 1, sizeof(screen), o);
    fclose(o);

    /* a quick checksum so frame changes can be compared in a script */
    uint32_t sum = 0;
    for (unsigned i = 0; i < sizeof(screen); i++) sum = sum * 31u + screen[i];
    printf("ran %d frames, cpu %u instructions / %u cycles, "
           "wrote %s (checksum %08X)\n",
           frames, cpu.instructions, cpu.cycles, name, sum);
    return 0;
}
