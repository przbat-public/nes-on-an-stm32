/*
 * ppu_expand_test.c — differential test for the background tile expander.
 *
 * The renderer used to expand a background tile one pixel at a time:
 *
 *     for (bit = 0; bit < 8; bit++) {
 *         uint8_t b = 7 - bit;
 *         uint8_t pix = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
 *         if (pix) out[bit] = (pix == 1) ? c1 : (pix == 2) ? c2 : c3;
 *     }
 *
 * with ppu_line pre-filled with pal_cache[0]. It now packs the two
 * bitplanes into a 2-bit-per-pixel word (sprd[]) and stores eight bytes
 * four nibbles at a time (pair[][]). That is a lot of table to get wrong
 * *silently*: the frame-for-frame comparisons only exercise the palettes
 * the test cartridges happen to write, and those set $3F00/$3F04/$3F08/
 * $3F0C to the same colour — which is exactly the case where a wrong rule
 * for pixel value 0 looks right.
 *
 * So this test compares the two implementations directly, exhaustively:
 * every (lo, hi) byte pair, every one of the four background palettes, and
 * a palette RAM whose entries are all different, including the palette
 * backdrops that the cartridge test ROMs never vary.
 *
 * The reference below is the old loop verbatim (it is the definition of
 * the project's rendering semantics — the frame comparisons were captured
 * against it); the "new" side is src/ppu.c itself, included so the test
 * builds the real tables through the real code path.
 *
 *   make host-ppu-test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../src/ppu.c"      /* the real build_pair(), sprd[], pair[][] */

/* ppu.c's renderer references these; this test never calls it */
uint8_t *nes_chr_ram;
int      nes_chr_is_ram;
const uint8_t *nes_chr[8];

/* the old per-pixel expansion, from ppu.c before the tables */
static void reference(const uint8_t *pal_cache, uint8_t backdrop,
                      uint8_t lo, uint8_t hi, int palette, uint8_t *out)
{
    const uint8_t *pc = &pal_cache[palette * 4];
    uint8_t c1 = pc[1], c2 = pc[2], c3 = pc[3];

    for (int i = 0; i < 8; i++) out[i] = backdrop;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t b = (uint8_t)(7 - bit);
        uint8_t pix = (uint8_t)(((lo >> b) & 1) | (((hi >> b) & 1) << 1));
        if (pix) out[bit] = (pix == 1) ? c1 : (pix == 2) ? c2 : c3;
    }
}

/* the new expansion, exactly as ppu_render_scanline does it */
static void new_way(uint8_t lo, uint8_t hi, int palette, uint8_t *out)
{
    uint32_t pat = sprd[lo] | ((uint32_t)sprd[hi] << 1);
    const uint16_t *pt = pair[palette];
    for (int j = 0; j < 4; j++) {
        uint16_t v = pt[(pat >> (4 * j)) & 0xF];
        out[2 * j]     = (uint8_t)(v & 0xFF);
        out[2 * j + 1] = (uint8_t)(v >> 8);
    }
}

int main(void)
{
    int checks = 0, failed = 0;

    /* a palette RAM where nothing coincides: $3F00/04/08/0C all differ, so
     * a wrong rule for pixel value 0 cannot hide */
    ppu_reset();
    for (int i = 0; i < 16; i++)
        ppu_write_vram((uint16_t)(0x3F00 + i), (uint8_t)(i + 1));
    for (int i = 0; i < 16; i++)
        ppu_write_vram((uint16_t)(0x3F10 + i), (uint8_t)(0x20 + i));

    uint8_t backdrop = pal_cache[0];
    printf("palette $3F00-3F0F:");
    for (int i = 0; i < 16; i++) printf(" %02X", ppu_read_vram((uint16_t)(0x3F00 + i)));
    printf("\n$3F00/04/08/0C = %02X %02X %02X %02X\n\n",
           ppu_read_vram(0x3F00), ppu_read_vram(0x3F04),
           ppu_read_vram(0x3F08), ppu_read_vram(0x3F0C));

    for (int palette = 0; palette < 4; palette++) {
        int bad = 0, shown = 0;
        for (int lo = 0; lo < 256; lo++) {
            for (int hi = 0; hi < 256; hi++) {
                uint8_t want[8], got[8];
                reference(pal_cache, backdrop, (uint8_t)lo, (uint8_t)hi,
                          palette, want);
                new_way((uint8_t)lo, (uint8_t)hi, palette, got);
                checks++;
                if (memcmp(want, got, 8) != 0) {
                    failed++; bad++;
                    if (shown++ < 3)
                        printf("  MISMATCH palette %d lo=%02X hi=%02X\n"
                               "    want %02X %02X %02X %02X %02X %02X %02X %02X\n"
                               "    got  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                               palette, lo, hi,
                               want[0], want[1], want[2], want[3],
                               want[4], want[5], want[6], want[7],
                               got[0], got[1], got[2], got[3],
                               got[4], got[5], got[6], got[7]);
                }
            }
        }
        printf("palette %d: %s (%d tile rows)\n", palette,
               bad ? "FAIL" : "ok", 65536);
    }

    printf("\n%d checks, %d failed — background expansion %s\n",
           checks, failed, failed ? "DIFFERS" : "is identical");
    return failed ? 1 : 0;
}
