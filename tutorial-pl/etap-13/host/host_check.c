/*
 * The check the chapter describes: run the emulated game for a while and
 * look at what came out.
 *
 * Build and run:
 *   cc -Wall -Wextra -I.. host_check.c ../cpu.c ../ppu.c -o check && ./check
 *
 * It is the same emulator as the stage, with no panel: main.c is included
 * with its main() renamed, so the bus, the cartridge and the frame loop are
 * exactly the ones the board runs.
 */
#include <stdio.h>
#include <stdint.h>

#define main stage_main_never_called
#include "../main.c"
#undef main

/* how many different colours a block of rows uses */
static int distinct_colours(int first_row, int rows)
{
    int seen[8] = {0};
    int count = 0;

    for (int y = first_row; y < first_row + rows; y++)
        for (int x = 0; x < PPU_W; x++)
            seen[framebuffer[(y * PPU_W) + x] & 7] = 1;
    for (int c = 0; c < 8; c++)
        count += seen[c];
    return count;
}

/* one palette entry, read back through the picture chip's own registers */
static int palette_entry(int index)
{
    bus_write(0x2006, 0x3F);
    bus_write(0x2006, (uint8_t)index);
    return bus_read(0x2007) & 31;
}

int main(void)
{
    int changes = 0;
    int previous = -1;

    machine_reset();

    for (int frame = 0; frame < 600; frame++) {
        render_frame();
        run_frame();

        if (stopped_opcode) {
            printf("frame %d: stopped at $%04X, opcode $%02X is not implemented\n",
                   frame, stopped_at, stopped_opcode);
            return 1;
        }
        if (frame < 3)
            continue;                        /* the boot is still filling memory */

        /* Bank 0's half is one solid tile, bank 1's half is a checkerboard,
         * so the two halves can never be made of the same thing. */
        if (distinct_colours(0, 120) != 1 || distinct_colours(120, 120) != 2) {
            printf("frame %d: top half uses %d colours, bottom half %d\n",
                   frame, distinct_colours(0, 120), distinct_colours(120, 120));
            return 1;
        }

        /* Each bank read the bank register and wrote that number, plus the
         * game's clock, into its own palette entry. The two entries must
         * therefore stay two apart, and both have to be inside the palette:
         * that only holds if each bank read its own number, not the other
         * bank's. */
        int top = palette_entry(1);
        int bottom = palette_entry(2);
        if (top > 15 || bottom > 15 || ((bottom - top) & 7) != 2) {
            printf("frame %d: the palette entries read %d and %d; two apart "
                   "and inside the palette was expected\n", frame, top, bottom);
            return 1;
        }

        if (previous >= 0 && top != previous)
            changes++;
        previous = top;
    }

    printf("600 frames: no unknown opcode; the top half one colour, the bottom\n"
           "half two; the top half's colour changed %d times\n", changes);
    printf("the two palette entries stayed two apart and inside the palette, so\n"
           "each bank read its own number back out of the window every frame\n");
    printf("cpu ran %u instructions, the bank register now says %u\n",
           cpu.instructions, bank);
    return 0;
}
