/*
 * mmc3_result.c — runs the MMC3 self-test cartridge for 90 frames and
 * prints the result byte the cartridge leaves in console RAM at $030F,
 * which must be 0x3F (one bit per sub-test).
 *
 *   cc -O2 -Wall -Wextra -Isrc -DNES_BUS_INLINE -o build/mmc3_result \
 *      tools/mmc3_result.c src/cpu6502.c src/ppu.c src/nes.c src/mapper.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/nes.h"
#include "../src/ppu.h"
#include "../src/cpu6502.h"

static uint8_t line[PPU_W];
static void line_hook(int y, const uint8_t *l) { (void)y; memcpy(line, l, PPU_W); }

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "build/mmc3.nes";
    int frames = (argc > 2) ? atoi(argv[2]) : 90;

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)len);
    if (fread(rom, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 1; }
    fclose(f);

    int rc = nes_load(rom, (uint32_t)len);
    if (rc != NES_OK) { printf("nes_load failed: %d\n", rc); return 1; }

    nes_line_hook = line_hook;
    nes_reset();
    for (int i = 0; i < frames; i++) nes_run_frame();

    uint8_t *ram = nes_ram();
    printf("after %d frames: RAM $0300..$030F =", frames);
    for (int i = 0; i < 16; i++) printf(" %02X", ram[0x300 + i]);
    printf("\n");
    printf("$030F = %02X  -> %s\n", ram[0x30F],
           ram[0x30F] == 0x3F ? "PASS (expected 3F)" : "FAIL (expected 3F)");
    return ram[0x30F] == 0x3F ? 0 : 1;
}
