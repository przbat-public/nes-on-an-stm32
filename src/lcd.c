/*
 * lcd.c — ST7789 driver for the NES picture.
 *
 * How this differs from a plain framebuffer driver:
 *   - the panel is addressed in landscape (MADCTL MV): a 320x240 window
 *     with the 256x240 NES picture centred. The side bars are painted
 *     black once at boot and never touched again;
 *   - the framebuffer holds only the NES picture (256x240 bytes, NES
 *     colour indices 0..63);
 *   - the picture leaves the chip in 4-scanline bands: each band is
 *     converted to the panel's pixel format in one of two staging buffers
 *     and handed to the SPI DMA. The conversion of a band happens while
 *     the previous band is still on the wire (the two buffers alternate),
 *     so the transfer of one band overlaps the emulation of the next.
 *
 * Two pixel formats, selected at compile time (-DLCD_12BIT):
 *
 *   16-bit (default)  RGB565, 2 bytes/pixel, COLMOD 0x55.
 *   12-bit           RGB444, 2 pixels in 3 bytes, COLMOD 0x53 — 25% less
 *                    wire time, 4 bits per channel instead of 5/6/5.
 */
#include "lcd.h"
#include "hal.h"
#include "font5x7.h"
#include <string.h>

#ifdef NES_PROFILING
#define CYC_NOW() cycles_now()      /* static inline in hal.h */
#else
#define CYC_NOW() 0u
#endif


/* display control pins (Arduino route on the GFX01M2 shield) */
#define PIN_CS  9    /* PA9  */
#define PIN_DC  10   /* PB10 */
#define PIN_RST 1    /* PA1  */

/* landscape orientation; if the picture comes out rotated 180 degrees
 * relative to the board, change this to 0xA0. */
#define MADCTL_LANDSCAPE 0x60

/* 4-byte aligned so the scanline copy and the change check can work a word
 * at a time (u32a: the word may alias the byte arrays, so say so) */
typedef uint32_t __attribute__((may_alias)) u32a;

static uint8_t  fb[FB_W * LCD_H] __attribute__((aligned(4)));  /* 256x240 */

/* diagnostics readable over SWD */
extern volatile uint32_t dbg_lines;
volatile uint32_t lcd_dbg_band_ok;
volatile uint32_t dbg_lcd_conv_ok;      /* set by the boot self-check   */
volatile uint32_t dbg_lcd_conv_checks;  /* how many bytes it compared   */
/* which pixel format this build drives the panel with (12 or 16); written
 * in lcd_init so that --gc-sections cannot drop it (nothing reads it) */
volatile uint32_t dbg_lcd_mode;

/* ------------------------------------------------------------ palette */
/* the 64 NES (2C02) colours as RGB888 — the same table as
 * tools/raw2png.py, so host renders and hardware agree */
static const uint8_t NES_RGB[64][3] = {
    { 84,  84,  84}, {  0,  30, 116}, {  8,  16, 144}, { 48,   0, 136},
    { 68,   0, 100}, { 92,   0,  48}, { 84,   4,   0}, { 60,  24,   0},
    { 32,  42,   0}, {  8,  58,   0}, {  0,  64,   0}, {  0,  60,   0},
    {  0,  50,  60}, {  0,   0,   0}, {  0,   0,   0}, {  0,   0,   0},
    {152, 150, 152}, {  8,  76, 196}, { 48,  50, 236}, { 92,  30, 228},
    {136,  20, 176}, {160,  20, 100}, {152,  34,  32}, {120,  60,   0},
    { 84,  90,   0}, { 40, 114,   0}, {  8, 124,   0}, {  0, 118,  40},
    {  0, 102, 120}, {  0,   0,   0}, {  0,   0,   0}, {  0,   0,   0},
    {236, 238, 236}, { 76, 154, 236}, {120, 124, 236}, {176,  98, 236},
    {228,  84, 236}, {236,  88, 180}, {236, 106, 100}, {212, 136,  32},
    {160, 170,   0}, {116, 196,   0}, { 76, 208,  32}, { 56, 204, 108},
    { 56, 180, 204}, { 60,  60,  60}, {  0,   0,   0}, {  0,   0,   0},
    {236, 238, 236}, {168, 204, 236}, {188, 188, 236}, {212, 178, 236},
    {236, 174, 236}, {236, 174, 212}, {236, 180, 176}, {228, 196, 144},
    {204, 210, 120}, {180, 222, 120}, {168, 226, 144}, {152, 226, 180},
    {160, 214, 228}, {160, 162, 160}, {  0,   0,   0}, {  0,   0,   0},
};

/* One channel of RGB888 -> one nibble. Round to nearest of the 16 levels:
 * the panel expands a nibble back to 6 bits by bit replication
 * (n -> n*4 + n/4, i.e. a linear 0..15 scale), so nearest is the right
 * inverse and halves the worst-case channel error against the 8-bit
 * source: 8/255 (3.1%) against 13/255 (5.1%) for truncation. */
#ifdef LCD_12BIT
static uint8_t nib4(uint8_t c) { return (uint8_t)((c * 15u + 127u) / 255u); }
#endif

#ifdef LCD_12BIT
/* 12-bit: the frame buffer is converted through these two tables, indexed
 * by a raw framebuffer byte like pal_sw_idx is in 16-bit mode.
 *
 * The panel takes two pixels in three bytes, six nibbles in order — pixel0
 * R,G,B then pixel1 R,G,B — with the high nibble first inside each byte
 * (ST7789V datasheet V1.2 §8.8.41, "Write data for 12-bit/pixel (RGB
 * 4-4-4-bit input)", the 4-line serial figure; the same six-nibble order
 * is in §8.8.2 for the parallel interface):
 *
 *   byte0 = p0.R p0.G     byte1 = p0.B p1.R     byte2 = p1.G p1.B
 *
 * So for pixels a (left) and b (right) the three bytes are the low three
 * bytes of a 32-bit word
 *
 *   x = (Ta << 12) | Tb          Ta/Tb = the 12-bit colour of a pixel
 *
 * stored little-endian, which packs as
 *
 *   x = ph12[a] | t12[b]
 *
 * with ph12 holding byte0 and p0.B, and t12 holding p1.R, p1.G and p1.B.
 * One 32-bit store per two pixels; the fourth byte it writes is the next
 * pair's byte0, so the staging buffers have four bytes of slack. */
static uint32_t ph12[256];
static uint32_t t12[256];
#else
static uint16_t pal[64];
/* pal_sw indexed by a raw framebuffer byte: the framebuffer only ever holds
 * masked NES colour indices, so this is pal_sw[b & 0x3F] without the mask
 * instruction in the conversion loop (51k pixels a frame) */
static uint16_t pal_sw_idx[256];
/* The ST7789 wants the high byte of each pixel first, so the staging buffer
 * holds byte-swapped RGB565. Swapping in the palette once per boot turns
 * the per-pixel conversion into a single 16-bit load and store. */
static uint16_t pal_sw[64];
#endif

/* Band staging: two buffers alternate, so the CPU converts the next band
 * while the DMA is still streaming the previous one out of the other. Only
 * ONE transfer can be in flight (SPI1_TX is wired to a single DMA channel
 * and starting a second transfer would overwrite the first one's
 * registers mid-flight), but the conversion no longer has to wait for it.
 * The extra word is slack for the 4-byte store the 12-bit loop uses. */
static uint32_t stage[2][(BAND_BYTES + 8) / 4];
static int      stage_cur;        /* buffer the next band is built in */
static int      dma_inflight;

/* What the panel already holds, band for band. A band that did not change
 * is neither converted nor sent, which saves both the CPU work and the
 * SPI traffic; the buffer starts as 0xFF, which is not a valid NES colour,
 * so the first frame sends everything. */
static uint8_t sent[NES_W * BAND_H] __attribute__((aligned(4)));
/* OR of (new scanline ^ sent) over the band being filled: zero means the
 * panel already has this band and it need not be converted or sent */
static uint32_t band_xor = 1;

/* The change check moved from a byte-wise scan of the band to a word-wise
 * OR accumulated while the scanlines are copied, so the firmware proves at
 * every boot that the two agree: for the first few frames it redoes the
 * slow byte-wise comparison and checks that it says "unchanged" exactly
 * when band_xor does, and that a band it just sent really did land in the
 * shadow. dbg_lcd_band_ok reads 1 over SWD when that held. */
static int check_bands = 8 * (LCD_H / BAND_H);
volatile uint32_t dbg_lcd_band_ok = 1, dbg_lcd_band_checked;

/* -------------------------------------------------- pixel conversion */

/* Convert `n` pixels (n is even) from NES colour indices to the panel's
 * byte stream. push_band() calls this for a whole band and the boot
 * self-check calls it for a handful of pixels, so the check exercises the
 * code that actually runs. `dst` needs 4 bytes of slack in 12-bit mode
 * (the last 32-bit store writes one byte past the last pixel pair). */
static void conv_pixels(const uint8_t *src, uint8_t *dst, int n)
{
#ifdef LCD_12BIT
    for (int i = 0; i < n; i += 2) {
        uint32_t x = ph12[src[i]] | t12[src[i + 1]];
        memcpy(dst, &x, 4);
        dst += 3;
    }
#else
    uint16_t *d = (uint16_t *)dst;
    for (int i = 0; i < n; i += 4) {
        d[i + 0] = pal_sw_idx[src[i + 0]];
        d[i + 1] = pal_sw_idx[src[i + 1]];
        d[i + 2] = pal_sw_idx[src[i + 2]];
        d[i + 3] = pal_sw_idx[src[i + 3]];
    }
#endif
}

/* A wrong packing cannot be caught by any host-side frame comparison: it
 * shows up only as wrong colours on the panel. So the firmware checks at
 * every boot that the bytes it is about to send are the ones the NES
 * palette table and the panel's data format call for, against two
 * expectations that do not come from the tables:
 *
 *   1. six hand-worked bytes for four pixels (two pairs, so the middle
 *      byte — which holds the low nibble of one pixel and the high nibble
 *      of the next — is exercised) written out from NES_RGB by hand;
 *   2. all 64 palette entries, with the expected nibbles recomputed here
 *      from NES_RGB through a different expression: (c+8)/17 against the
 *      table builder's (c*15+127)/255. Both are exact round-half-up of
 *      c/17 (an integer boundary cannot fall between them), but they are
 *      different spellings, and the hand-worked bytes of (1) anchor them.
 *
 * dbg_lcd_conv_checks counts the byte comparisons, dbg_lcd_conv_ok is 1
 * only if all of them matched. */
static void lcd_conv_selfcheck(void)
{
    uint32_t checks = 0, ok = 1;
    uint8_t got[24];

#ifdef LCD_12BIT
    static const uint8_t src[4]  = { 0x0F, 0x21, 0x30, 0x16 };
    static const uint8_t want[6] = { 0x00, 0x04, 0x9E, 0xEE, 0xE9, 0x22 };

    /* (1) the real conversion, four pixels = two pairs = six bytes */
    conv_pixels(src, got, 4);
    for (int i = 0; i < 6; i++) {
        checks++;
        if (got[i] != want[i]) ok = 0;
    }

    /* (2) every entry of the table, and the packing, against NES_RGB */
    for (int i = 0; i < 64; i++) {
        int j = (i + 1) & 63;
        uint8_t pix[2] = { (uint8_t)i, (uint8_t)j };
        uint8_t r0 = (uint8_t)((NES_RGB[i][0] + 8) / 17);
        uint8_t g0 = (uint8_t)((NES_RGB[i][1] + 8) / 17);
        uint8_t b0 = (uint8_t)((NES_RGB[i][2] + 8) / 17);
        uint8_t r1 = (uint8_t)((NES_RGB[j][0] + 8) / 17);
        uint8_t g1 = (uint8_t)((NES_RGB[j][1] + 8) / 17);
        uint8_t b1 = (uint8_t)((NES_RGB[j][2] + 8) / 17);
        uint8_t exp[3];
        exp[0] = (uint8_t)((r0 << 4) | g0);
        exp[1] = (uint8_t)((b0 << 4) | r1);
        exp[2] = (uint8_t)((g1 << 4) | b1);
        conv_pixels(pix, got, 2);
        for (int k = 0; k < 3; k++) {
            checks++;
            if (got[k] != exp[k]) ok = 0;
        }
    }
#else
    static const uint8_t src[4]  = { 0x0F, 0x21, 0x30, 0x16 };
    static const uint8_t want[8] = { 0x00, 0x00, 0x4C, 0xDD,
                                     0xEF, 0x7D, 0x99, 0x04 };

    /* (1) the real conversion, four pixels = eight bytes */
    conv_pixels(src, got, 4);
    for (int i = 0; i < 8; i++) {
        checks++;
        if (got[i] != want[i]) ok = 0;
    }

    /* (2) every entry of the table the band conversion indexes */
    for (int i = 0; i < 64; i++) {
        uint8_t pix[4] = { (uint8_t)i, (uint8_t)i, (uint8_t)i, (uint8_t)i };
        uint16_t c = (uint16_t)(((NES_RGB[i][0] >> 3) << 11)
                                | ((NES_RGB[i][1] >> 2) << 5)
                                | (NES_RGB[i][2] >> 3));
        uint8_t exp[2] = { (uint8_t)(c >> 8), (uint8_t)c };
        conv_pixels(pix, got, 4);
        for (int k = 0; k < 2; k++) {
            checks++;
            if (got[2 * 0 + k] != exp[k]) ok = 0;
        }
        /* the table is indexed by a raw framebuffer byte: check the mask,
         * too, by converting one pixel of every quarter of the table */
        uint8_t raw = (uint8_t)(i + 64);
        uint8_t pix2[2] = { raw, raw };
        conv_pixels(pix2, got, 2);
        for (int k = 0; k < 2; k++) {
            checks++;
            if (got[k] != exp[k]) ok = 0;
        }
    }
#endif

    dbg_lcd_conv_checks = checks;
    dbg_lcd_conv_ok = ok;
}

/* ------------------------------------------------------- ST7789 glue */

static void cs(int on) { if (on) gpio_set(PORT_A, PIN_CS); else gpio_clear(PORT_A, PIN_CS); }
static void dc(int on) { if (on) gpio_set(PORT_B, PIN_DC); else gpio_clear(PORT_B, PIN_DC); }

static void cmd(uint8_t c) { dc(0); cs(0); spi_write(&c, 1); cs(1); }

static void data(const uint8_t *d, uint32_t n) { dc(1); cs(0); spi_write(d, n); cs(1); }

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];
    cmd(0x2A);
    b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    data(b, 4);
    cmd(0x2B);
    b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    data(b, 4);
    cmd(0x2C);
}

/* one-off CPU-driven push of a rectangle (boot screen, fps overlay).
 * Uses the same conversion function as the band path, so every path that
 * pushes pixels packs them the same way. */
static void push_rect(int x0, int y0, int w, int h)
{
#ifdef LCD_12BIT
    /* worst case one full-width row of packed pixels, plus slack */
    static uint8_t line[NES_W * 3 / 2 + 8];
    int wp = w + (w & 1);            /* the panel takes pixels in pairs */
#else
    static uint8_t line[NES_W * 2 + 8];
    /* the converter works four pixels at a time; only the first w*2 bytes
     * of the buffer are sent, so a width that is not a multiple of four
     * costs a few converted pixels of padding and nothing else */
    int wp = w;
#endif
    set_window((uint16_t)(NES_X + x0), (uint16_t)y0,
               (uint16_t)(NES_X + x0 + wp - 1), (uint16_t)(y0 + h - 1));
    dc(1); cs(0);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = fb + (uint32_t)(y0 + y) * FB_W + x0;
        conv_pixels(row, line, wp);
        spi_write(line, (uint32_t)wp * LCD_BPP / 8);
    }
    cs(1);
}

void lcd_init(void)
{
    uint8_t v;

    dbg_lcd_mode = LCD_BPP;

#ifdef LCD_12BIT
    for (int i = 0; i < 64; i++) {
        uint8_t r = nib4(NES_RGB[i][0]);
        uint8_t g = nib4(NES_RGB[i][1]);
        uint8_t b = nib4(NES_RGB[i][2]);
        /* see the comment on ph12/t12 for what these two words hold */
        ph12[i] = (uint32_t)((r << 4) | g | (b << 12));
        t12[i]  = (uint32_t)((r << 8) | (b << 16) | (g << 20));
    }
    for (int i = 64; i < 256; i++) {   /* mask once, as pal_sw_idx does */
        ph12[i] = ph12[i & 0x3F];
        t12[i]  = t12[i & 0x3F];
    }
#else
    for (int i = 0; i < 64; i++) {
        uint16_t c = (uint16_t)(((NES_RGB[i][0] >> 3) << 11)
                                | ((NES_RGB[i][1] >> 2) << 5)
                                | (NES_RGB[i][2] >> 3));
        pal[i] = c;
        pal_sw[i] = (uint16_t)((c >> 8) | (c << 8));
    }
    for (int i = 0; i < 256; i++)
        pal_sw_idx[i] = pal_sw[i & 0x3F];
#endif
    lcd_conv_selfcheck();

    for (uint32_t i = 0; i < sizeof(fb); i++) fb[i] = 0x0F;   /* black */
    check_bands = 8 * (LCD_H / BAND_H);
    dbg_lcd_band_ok = 1;
    dbg_lcd_band_checked = 0;
    memset(sent, 0xFF, sizeof(sent));     /* 0xFF is not a NES colour: the
                                           * first frame sends everything */

    gpio_clear(PORT_A, PIN_RST); delay_ms(20);
    gpio_set(PORT_A, PIN_RST);   delay_ms(120);

    cmd(0x36); v = MADCTL_LANDSCAPE; data(&v, 1);
    cmd(0x3A);
#ifdef LCD_12BIT
    v = 0x53;                          /* 12 bits/pixel, RGB444, 4K colours */
#else
    v = 0x55;                          /* 16 bits/pixel, RGB565, 65K colours */
#endif
    data(&v, 1);
    cmd(0x11); delay_ms(120);                  /* sleep out */
    cmd(0x29); delay_ms(20);                   /* display on */

    spi_dma_init();

    /* black out the whole panel once (this also paints the side bars).
     * All-zero bytes are the same black in either format, and a padded
     * buffer keeps this to a few hundred transfers instead of 76,800. */
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    dc(1); cs(0);
    {
        uint8_t zeros[96];
        memset(zeros, 0, sizeof(zeros));
        uint32_t total = (uint32_t)LCD_W * LCD_H * LCD_BPP / 8;
        while (total) {
            uint32_t n = total > sizeof(zeros) ? sizeof(zeros) : total;
            spi_write(zeros, n);
            total -= n;
        }
    }
    cs(1);
}

/* --------------------------------------------------------- rendering */

void lcd_clear(uint8_t color_index)
{
    for (uint32_t i = 0; i < sizeof(fb); i++) fb[i] = color_index;
}

void lcd_text(int16_t x, int16_t y, const char *s, uint8_t fg, uint8_t bg)
{
    while (*s) {
        if (*s < 32 || *s > 126) { s++; continue; }
        const uint8_t *g = Font5x7[(uint8_t)(*s - 32)];
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t line = g[col];
            for (uint8_t row = 0; row < 7; row++) {
                int16_t px = x + col, py = y + row;
                if (px >= 0 && px < FB_W && py >= 0 && py < LCD_H)
                    fb[py * FB_W + px] = (line & 1) ? fg : bg;
                line >>= 1;
            }
        }
        x += 6;
        s++;
    }
}

void lcd_push_full(void)
{
    push_rect(0, 0, NES_W, LCD_H);
}

void lcd_show_fps(int fps)
{
    char buf[8];
    int n = 0;
    if (fps >= 100) buf[n++] = (char)('0' + fps / 100);
    if (fps >= 10)  buf[n++] = (char)('0' + (fps / 10) % 10);
    buf[n++] = (char)('0' + fps % 10);
    buf[n++] = 'F'; buf[n++] = 'P'; buf[n++] = 'S';
    buf[n] = 0;
    lcd_text(0, 1, buf, 32, 15);        /* top-left corner of the picture */
    push_rect(0, 0, 48, 9);             /* send just that corner */
}

/* ------------------------------------------------------ SPI streaming */

/* Where a frame goes, published once per frame (lcd_dbg_frame) so that a
 * single SWD read is a complete, self-consistent frame — the accumulators
 * are ordinary statics, the dbg_* values are the last finished frame. */
volatile uint32_t dbg_cyc_flush, dbg_cyc_band, dbg_cyc_wait;
volatile uint32_t dbg_cyc_diff, dbg_cyc_conv, dbg_cyc_setwin;
volatile uint32_t dbg_cyc_copy;                /* scanline copy in fb */
volatile uint32_t dbg_cyc_frameend;            /* the last band's wait */
volatile uint32_t dbg_bands_sent, dbg_bands_skipped;
static uint32_t acc_flush, acc_band, acc_wait, acc_diff, acc_conv;
static uint32_t acc_setwin, acc_copy, acc_frameend, acc_sent, acc_skipped;

void lcd_dbg_frame(void)
{
    dbg_cyc_flush    = acc_flush;    acc_flush    = 0;
    dbg_cyc_band     = acc_band;     acc_band     = 0;
    dbg_cyc_wait     = acc_wait;     acc_wait     = 0;
    dbg_cyc_diff     = acc_diff;     acc_diff     = 0;
    dbg_cyc_conv     = acc_conv;     acc_conv     = 0;
    dbg_cyc_setwin   = acc_setwin;   acc_setwin   = 0;
    dbg_cyc_copy     = acc_copy;     acc_copy     = 0;
    dbg_cyc_frameend = acc_frameend; acc_frameend = 0;
    dbg_bands_sent   = acc_sent;     acc_sent     = 0;
    dbg_bands_skipped = acc_skipped; acc_skipped  = 0;
}

static void push_band(int y0)
{
    const uint8_t *src = fb + (uint32_t)y0 * FB_W;

    uint32_t t = CYC_NOW();
    bool checking = check_bands > 0;
    if (checking) check_bands--;
    uint32_t slow_diff = 0;
    if (checking) {
        for (int i = 0; i < NES_W * BAND_H; i++)
            slow_diff |= (uint32_t)(src[i] ^ sent[i]);
        dbg_lcd_band_checked++;
        if ((slow_diff == 0) != (band_xor == 0)) dbg_lcd_band_ok = 0;
    }
    /* band_xor was accumulated by the scanline copies that make up this
     * band, so "did anything change" costs nothing extra here */
    if (band_xor == 0) {
        acc_skipped++;
        acc_diff += CYC_NOW() - t;
        return;                       /* the panel is already showing this */
    }
    band_xor = 0;
    memcpy(sent, src, sizeof(sent));
    if (checking) {
        uint32_t d = 0;
        for (int i = 0; i < NES_W * BAND_H; i++)
            d |= (uint32_t)(src[i] ^ sent[i]);
        if (d != 0) dbg_lcd_band_ok = 0;      /* the shadow must match now */
    }
    acc_sent++;
    acc_diff += CYC_NOW() - t;

    /* Convert into the buffer the running transfer is NOT reading, so this
     * runs while the previous band is still going out on the wire. */
    uint32_t t0 = CYC_NOW();
    uint8_t *dst = (uint8_t *)stage[stage_cur];
    conv_pixels(src, dst, NES_W * BAND_H);

    /* the previous band must be off the wire before we reuse its buffer
     * (the conversion above has had a whole band's worth of time to hide
     * in, so this usually returns immediately), and before we send the
     * commands of the next window */
    uint32_t t1 = CYC_NOW();
    acc_conv += t1 - t0;
    while (dma_inflight > 0) {
        spi_dma_wait();
        dma_inflight = 0;
    }
    uint32_t t2 = CYC_NOW();
    acc_wait += t2 - t1;
    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    if (spi_dma_available()) {
        spi_dma_start(dst, BAND_BYTES);
        dma_inflight = 1;
        stage_cur ^= 1;
        lcd_dbg_band_ok = 1;
    } else {
        /* no DMA: shift the band out with the CPU */
        spi_write(dst, BAND_BYTES);
        cs(1);
    }
    acc_setwin += CYC_NOW() - t2;
    acc_band   += CYC_NOW() - t0;
}


/* The PPU renders its scanlines straight into these rows (nes.c asks for
 * the row before rendering), so the picture is written once instead of
 * into a line buffer here and then copied: that copy was 61,440 bytes, or
 * about 2 ms, of every frame. */
uint8_t *lcd_nes_line_target(int y)
{
    return fb + (uint32_t)y * FB_W;
}

void lcd_nes_line(int y, const uint8_t *line)
{
    uint32_t t0 = CYC_NOW();
    const uint8_t *dst = fb + (uint32_t)y * FB_W;
    /* the shadow of this band's scanline, what the panel currently holds */
    const uint8_t *sh = sent + (uint32_t)(y & (BAND_H - 1)) * NES_W;
    dbg_lines = (uint32_t)y;

    /* a caller that did not render into the framebuffer row still gets the
     * picture (the host tools do, and nes_line_target may be unset) */
    if (line != dst)
        memcpy((uint8_t *)dst, line, NES_W);

    /* one pass over the two rows: does this scanline differ from what the
     * panel was last given? (the byte-wise check that used to run over the
     * whole band at push time is folded in here) */
    for (int x = 0; x < NES_W; x += 8) {
        u32a a0, a1, b0, b1;
        memcpy(&a0, dst + x, 4);
        memcpy(&a1, dst + x + 4, 4);
        memcpy(&b0, sh + x, 4);
        memcpy(&b1, sh + x + 4, 4);
        band_xor |= (a0 ^ b0) | (a1 ^ b1);
    }

    uint32_t t1 = CYC_NOW();
    acc_copy += t1 - t0;

    if ((y & (BAND_H - 1)) == BAND_H - 1)
        push_band(y - (BAND_H - 1));

    acc_flush += CYC_NOW() - t0;
}

void lcd_nes_frame_end(void)
{
    uint32_t t = CYC_NOW();
    while (dma_inflight > 0) {
        spi_dma_wait();
        dma_inflight--;
    }
    cs(1);
    acc_frameend += CYC_NOW() - t;
}
