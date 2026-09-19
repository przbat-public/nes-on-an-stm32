/*
 * Host harness for stage 13, not part of the lesson.
 *
 * The stage's own main.c talks to a panel over SPI, so on its own it can only
 * be compiled, not run. This file includes it with main() renamed and puts a
 * different main() underneath: the same emulator, the same cartridge, but the
 * picture goes to a file and the frame counter to the terminal. That is how
 * the chapter's "what you should see" was checked.
 *
 *   cc -Wall -Wextra -I.. host_main.c -o host_run
 *   ./host_run            # text preview plus a checksum
 *   ./host_run out.ppm    # and a picture you can look at
 */

#define main stage_main_never_called
#include "main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *ppm = (argc > 1) ? argv[1] : NULL;
    int frames = (argc > 2) ? atoi(argv[2]) : 30;

    machine_reset();
    if (getenv("TRACE")) {
        for (int i = 0; i < 400; i++) {
            printf("%04X  A=%02X X=%02X Y=%02X P=%02X  op=%02X\n",
                   cpu.pc, cpu.a, cpu.x, cpu.y, cpu.status, bus_read(cpu.pc));
            if (!cpu_step()) { printf("stopped\n"); break; }
        }
        return 0;
    }

    for (int f = 0; f < frames; f++) {
        render_frame();
        run_frame();
    }
    render_frame();

    /* What the picture chip holds, as palette numbers. Every eighth pixel is
     * enough to see the shape. */
    printf("picture after %d frames, top left corner sampled:\n", frames);
    for (int y = 8; y < PPU_H; y += 40) {
        for (int x = 8; x < PPU_W; x += 8)
            putchar('0' + (framebuffer[y * PPU_W + x] & 7));
        putchar('\n');
    }

    printf("bank register: %u, cpu: %u instructions\n",
           bank, cpu.instructions);
    if (stopped_opcode)
        printf("the processor stopped at $%04X: opcode $%02X is not implemented\n",
               stopped_at, stopped_opcode);

    if (ppm) {
        FILE *out = fopen(ppm, "wb");
        if (!out) {
            perror(ppm);
            return 1;
        }
        fprintf(out, "P6\n%d %d\n255\n", PPU_W, PPU_H);
        for (int i = 0; i < PPU_W * PPU_H; i++) {
            uint16_t c = panel_colours[framebuffer[i] & 7];
            unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                (unsigned char)((c & 0x1F) * 255 / 31),
            };
            fwrite(rgb, 1, 3, out);
        }
        fclose(out);
        printf("wrote %s\n", ppm);
    }
    return 0;
}
