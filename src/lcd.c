/*
 * lcd.c — ST7789 driver for the NES picture.
 *
 * How this differs from a plain framebuffer driver:
 *   - the panel is addressed in landscape (MADCTL MV): a 320x240 window
 *     with the 256x240 NES picture centred. The side bars are painted
 *     black once at boot and never touched again;
 *   - the framebuffer holds only the NES picture (256x240 bytes, NES
 *     colour indices 0..63);
 *   - the picture leaves the chip in 8-scanline bands: each band is
 *     converted to big-endian RGB565 in a staging buffer and handed to
 *     the SPI DMA. Two staging buffers alternate, so the transfer of one
 *     band overlaps the PPU rendering the next one — the wire time
 *     (~13 ms/frame at 40 MHz) hides behind the emulation.
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
static uint16_t pal[64];
/* pal_sw indexed by a raw framebuffer byte: the framebuffer only ever holds
 * masked NES colour indices, so this is pal_sw[b & 0x3F] without the mask
 * instruction in the conversion loop (51k pixels a frame) */
static uint16_t pal_sw_idx[256];
/* The ST7789 wants the high byte of each pixel first, so the staging buffer
 * holds byte-swapped RGB565. Swapping in the palette once per boot turns
 * the per-pixel conversion into a single 16-bit load and store. */
static uint16_t pal_sw[64];
volatile uint32_t dbg_lcd_conv_ok;   /* set by the boot self-check */

/* Band staging (4 KB): the CPU converts a band here, the DMA streams it
 * out while the PPU renders the next band. Only ONE transfer can be in
 * flight: SPI1_TX is wired to a single DMA channel, and starting a second
 * transfer would overwrite the first one's registers mid-flight. */
static uint16_t stage[BAND_BYTES / 2];   /* 16-bit aligned on purpose */
static int     dma_inflight;

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

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

/* one-off CPU-driven push of a rectangle (boot screen, fps overlay) */
static void push_rect(int x0, int y0, int w, int h)
{
    set_window((uint16_t)(NES_X + x0), (uint16_t)y0,
               (uint16_t)(NES_X + x0 + w - 1), (uint16_t)(y0 + h - 1));
    dc(1); cs(0);
    uint8_t pair[2];
    for (int y = 0; y < h; y++) {
        const uint8_t *row = fb + (uint32_t)(y0 + y) * FB_W + x0;
        for (int x = 0; x < w; x++) {
            uint16_t c = pal[row[x] & 0x3F];
            pair[0] = (uint8_t)(c >> 8);
            pair[1] = (uint8_t)(c & 0xFF);
            spi_write(pair, 2);
        }
    }
    cs(1);
}

/* A wrong byte order in the band conversion shows up as wrong colours on
 * the panel and nowhere else, so the firmware checks the conversion once
 * at boot against bytes worked out from the NES palette by hand. */
static void lcd_conv_selfcheck(void)
{
    static const uint8_t src[4]   = { 0x0F, 0x21, 0x30, 0x16 };
    static const uint8_t want[8]  = { 0x00, 0x00, 0x4C, 0xDD,
                                      0xEF, 0x7D, 0x99, 0x04 };
    uint16_t got16[4];
    const uint8_t *got = (const uint8_t *)got16;

    /* pal_sw_idx, not pal_sw: that is the table the band conversion uses */
    for (int i = 0; i < 4; i++)
        got16[i] = pal_sw_idx[src[i]];

    dbg_lcd_conv_ok = 1;
    for (int i = 0; i < 8; i++)
        if (got[i] != want[i]) dbg_lcd_conv_ok = 0;
}

void lcd_init(void)
{
    uint8_t v;

    for (int i = 0; i < 64; i++) {
        pal[i] = rgb565(NES_RGB[i][0], NES_RGB[i][1], NES_RGB[i][2]);
        pal_sw[i] = (uint16_t)((pal[i] >> 8) | (pal[i] << 8));
    }
    for (int i = 0; i < 256; i++)
        pal_sw_idx[i] = pal_sw[i & 0x3F];
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
    cmd(0x3A); v = 0x55; data(&v, 1);          /* 16 bpp */
    cmd(0x11); delay_ms(120);                  /* sleep out */
    cmd(0x29); delay_ms(20);                   /* display on */

    spi_dma_init();

    /* black out the whole panel once (this also paints the side bars) */
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    dc(1); cs(0);
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        uint8_t pair[2] = { 0, 0 };
        spi_write(pair, 2);
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
    /* band_xor was accumulated by the eight scanline copies that make up
     * this band, so "did anything change" costs nothing extra here */
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

    uint32_t t0 = CYC_NOW();
    /* the previous band must be off the wire before we reuse the buffer
     * (it has had a whole band's worth of rendering time to finish) */
    while (dma_inflight > 0) {
        spi_dma_wait();
        dma_inflight--;
    }
    acc_wait += CYC_NOW() - t0;
    t0 = CYC_NOW();

    uint16_t *dst = stage;

    for (int i = 0; i < NES_W * BAND_H; i += 4) {
        dst[i + 0] = pal_sw_idx[src[i + 0]];
        dst[i + 1] = pal_sw_idx[src[i + 1]];
        dst[i + 2] = pal_sw_idx[src[i + 2]];
        dst[i + 3] = pal_sw_idx[src[i + 3]];
    }

    uint32_t t1 = CYC_NOW();
    acc_conv += t1 - t0;

    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    if (spi_dma_available()) {
        spi_dma_start((const uint8_t *)stage, BAND_BYTES);
        dma_inflight = 1;
        lcd_dbg_band_ok = 1;
    } else {
        /* no DMA: shift the band out with the CPU */
        spi_write((const uint8_t *)stage, BAND_BYTES);
        cs(1);
    }
    uint32_t t2 = CYC_NOW();
    acc_setwin += t2 - t1;
    acc_band   += t2 - t0;
}


void lcd_nes_line(int y, const uint8_t *line)
{
    uint32_t t0 = CYC_NOW();
    uint8_t *dst = fb + (uint32_t)y * FB_W;
    /* the shadow of this band's scanline, what the panel currently holds */
    const uint8_t *sh = sent + (uint32_t)(y & (BAND_H - 1)) * NES_W;
    dbg_lines = (uint32_t)y;

    /* One pass instead of three: copy the scanline into the framebuffer
     * and note whether it differs from what was last sent, four pixels at
     * a time. The change check used to be a separate byte-wise scan of the
     * whole band at push time, and the copy a byte-wise loop here. */
    for (int x = 0; x < NES_W; x += 8) {
        u32a a0, a1, b0, b1;
        memcpy(&a0, line + x, 4);
        memcpy(&a1, line + x + 4, 4);
        memcpy(&b0, sh + x, 4);
        memcpy(&b1, sh + x + 4, 4);
        band_xor |= (a0 ^ b0) | (a1 ^ b1);
        memcpy(dst + x, &a0, 4);
        memcpy(dst + x + 4, &a1, 4);
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
