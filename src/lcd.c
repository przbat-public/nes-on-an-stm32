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

/* ------------------------------------- what the panel holds, band by band */
/*
 * A band is sent if and only if it differs from what the panel was last
 * given. That decision has to be exact — a band wrongly skipped is a stale
 * or duplicated stripe on the panel, and nothing in the framebuffer shows
 * it.
 *
 * The obvious way to do it is to keep a copy of what the panel holds (a
 * 61,440-byte shadow) and compare the 1 KB band against it word-wise. That
 * does not fit on this chip: the framebuffer is 60 KB of SRAM1's 96 KB, the
 * mappers' 8 KB work RAM and 8 KB CHR RAM are another 16 KB, and the
 * staging buffers and tables take the rest — a second full frame would
 * need 146 KB of a 128 KB part (SRAM1+SRAM2), so it cannot be linked at
 * any -O level.
 *
 * The shadow is not needed, because at the moment a band is *started* the
 * framebuffer still holds exactly what the panel holds: the previous frame
 * ended with the two in step (every band that changed was pushed), and the
 * only thing that happens between frames — the fps overlay — writes the
 * framebuffer and the panel together. So one band's worth of copy, taken
 * just before the renderer overwrites it, is enough:
 *
 *     panel == framebuffer          at the start of a band
 *     band changed  <=>  framebuffer_band_now != the copy
 *
 * which is the same test as "differs from what the panel holds", with 1 KB
 * of RAM instead of 60 KB. The copy costs a 1 KB memcpy per band (the
 * comparison costs another 1 KB read per band); both are timed into
 * dbg_cyc_copy / dbg_cyc_diff and measured on the board.
 *
 * Nothing may write into a band of the framebuffer between its snapshot
 * and its push except the renderer drawing that band: that would make the
 * snapshot disagree with the panel. In this firmware nothing does (the
 * overlay paths write and push together, between frames).
 */
static uint8_t held[NES_W * BAND_H] __attribute__((aligned(4)));

/* The first frame must send everything: the panel was filled black at boot
 * and there is no history to compare against. Also set by dbg_lcd_resync,
 * which re-sends the whole picture once (recovery, and a way to check that
 * a suspect panel is being refreshed at all). */
static int force_send = 1;
volatile uint32_t dbg_lcd_resync;
volatile uint32_t dbg_lcd_forced;      /* frames that were force-sent */

/* ------------------------------- the referee (a checksum-based inspector) */
/*
 * The decision above trusts one thing it cannot see: that the copy really
 * is what the panel holds. This referee is the independent check the
 * hardware allows (the panel's own memory cannot be read back — see the
 * readback section below).
 *
 * It keeps one 32-bit fingerprint per band of the content last handed to
 * the panel, and every time a band is *skipped* it requires the current
 * band's fingerprint to equal the stored one. A wrong reference (comparing
 * against another band, or a shadow that was never updated — the bug this
 * code had) makes a changed band look unchanged and is caught here with
 * probability 1 - 2^-32 per band. It is a referee, not the decision: the
 * decision stays the exact byte comparison.
 *
 * dbg_lcd_invariant_checks counts the skips it has judged,
 * dbg_lcd_invariant_bad the ones that failed, and dbg_lcd_invariant_ok is
 * 1 only while nothing has failed. It costs a fingerprint of every band
 * (~1.5 ms a frame, visible in dbg_cyc_diff), so it runs only through the
 * boot window, while dbg_lcd_stress is running and while dbg_lcd_check_req
 * is left set over SWD — which is also how its cost was measured.
 */
static uint32_t sent_fp[LCD_H / BAND_H];
static int      referee;                   /* on: boot window, on demand, stress */
volatile uint32_t dbg_lcd_check_req;       /* SWD: 1 keeps the referee running */
volatile uint32_t dbg_lcd_invariant_ok = 1;
volatile uint32_t dbg_lcd_invariant_checks;
volatile uint32_t dbg_lcd_invariant_bad;

/* The boot window: for the first few frames every band is also checked the
 * slow, byte-wise way (a full read of both buffers), so the word-wise loop
 * that makes the decision is shown to agree with an obvious one. */
static int check_bands = 8 * (LCD_H / BAND_H);
volatile uint32_t dbg_lcd_band_ok = 1, dbg_lcd_band_checked;

static uint32_t band_fp(const uint8_t *p)
{
    const u32a *w = (const u32a *)(const void *)p;
    uint32_t x = 0, s = 0;
    for (int i = 0; i < (NES_W * BAND_H) / 4; i++) {
        uint32_t v = w[i];
        x ^= v;
        s += v;
    }
    return x ^ (s * 2654435761u);
}

/* Do the two bands differ? Both are 1 KB, contiguous and 4-byte aligned;
 * 60 bands a frame means 120 KB read, so the loop moves 32 bytes (eight
 * words) per iteration instead of branching once per word. What it costs
 * on the board is dbg_cyc_diff: 1.16 ms a frame against the 1.47 ms the
 * per-line heuristic it replaced cost (see docs/PERFORMANCE.md). */
static int band_differs(const uint8_t *a8, const uint8_t *b8)
{
    const u32a *a = (const u32a *)(const void *)a8;
    const u32a *b = (const u32a *)(const void *)b8;
    uint32_t d = 0;

    for (int i = 0; i < (NES_W * BAND_H) / 32; i++) {
        uint32_t x[8], y[8];
        memcpy(x, a + 8 * i, sizeof(x));
        memcpy(y, b + 8 * i, sizeof(y));
        d |= (x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])
           | (x[4] ^ y[4]) | (x[5] ^ y[5]) | (x[6] ^ y[6]) | (x[7] ^ y[7]);
    }
    return d != 0;
}

/* What the panel holds for band `b` is now the framebuffer's content: used
 * when something writes both at once (push_rect, the whole-picture push). */
static void referee_note_band(int b)
{
    if (referee)
        sent_fp[b] = band_fp(fb + (uint32_t)b * BAND_H * FB_W);
}

static void referee_seed(void)
{
    for (int b = 0; b < LCD_H / BAND_H; b++)
        sent_fp[b] = band_fp(fb + (uint32_t)b * BAND_H * FB_W);
}

/* ------------------------------- the stress test (a hardware-only proof) */
/*
 * The referee judges the decisions the firmware makes; this makes it make
 * the decisions the reported stripes came from, on demand (dbg_lcd_stress
 * is a frame budget written over SWD).
 *
 * Every frame it paints a known block into one band of the lower half of
 * the picture *and pushes it to the panel*, so the panel really holds the
 * artificial content — and the next frame the emulator draws its own
 * picture over that band, which therefore has to be sent back. Two
 * counters watch that: dbg_lcd_stress_missed counts an injected band that
 * was not sent in the next frame (a band that changed and was skipped),
 * and the referee independently requires every skipped band to still match
 * what the panel was given (dbg_lcd_invariant_bad).
 *
 * It is the failure condition the report describes — a static part of the
 * screen, a band given one content and then asked to go back to what its
 * neighbours look like — without needing the game to cooperate.
 *
 * The block is 48x4 pixels in one band, so on the panel it reads as a
 * short flashing bar; that is the diagnostic being visible, and it stops
 * as soon as the budget runs out.
 */
volatile uint32_t dbg_lcd_stress;          /* frames left, written over SWD */
volatile uint32_t dbg_lcd_stress_frames;   /* injections done so far        */
volatile uint32_t dbg_lcd_stress_missed;   /* injected band not sent back   */
static int      stress_watch = -1;         /* the band injected last frame  */
static int      stress_seen;               /* ... and whether it was sent   */

static void push_rect(int x0, int y0, int w, int h);   /* further down */

static void stress_tick(void)
{
    if (stress_watch >= 0 && !stress_seen)
        dbg_lcd_stress_missed++;
    stress_watch = -1;
    stress_seen = 0;

    if (!dbg_lcd_stress)
        return;
    dbg_lcd_stress--;
    dbg_lcd_stress_frames++;

    uint32_t n = dbg_lcd_stress_frames;
    int b  = 40 + (int)(n % 16);                  /* lower half: 40..55 */
    int x0 = 16 + (int)((n * 37u) % 200u);
    int y0 = b * BAND_H;
    uint8_t c = (uint8_t)((n * 11u) & 0x3F);
    int painted = 0;

    for (int y = y0; y < y0 + BAND_H; y++) {
        for (int x = x0; x < x0 + 48; x++) {
            uint8_t *p = &fb[(uint32_t)y * FB_W + x];
            if (*p != c) { *p = c; painted++; }
        }
    }
    push_rect(x0, y0, 48, BAND_H);        /* the panel gets it too */

    /* Watch this band only if the paint really changed it: a block that
     * already held that colour changed nothing, so there is nothing that
     * has to be sent back and counting it would report a miss that is not
     * one. (On a dungeon screen with large flat areas that is roughly one
     * injection in a hundred.) */
    stress_watch = painted ? b : -1;
}

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

/* The address window, without starting a write: the picture path adds
 * 0x2C (RAMWR) and the readback path in this file adds 0x2E (RAMRD),
 * which needs the window set and the read pointer left at its origin. */
static void set_window_addr(uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1)
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
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    set_window_addr(x0, y0, x1, y1);
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

    /* The panel and the framebuffer have just been given the same pixels
     * for this rectangle; since the two were in step before, they still
     * are for the whole bands it touches, so the referee's view of those
     * bands is simply the framebuffer's. (This is the fps overlay and the
     * stress test; the picture itself never comes through here.) */
    for (int b = y0 / BAND_H; b <= (y0 + h - 1) / BAND_H; b++)
        referee_note_band(b);
}

/* --------------------------------------------------- panel readback */
/*
 * Everything else in this file checks the firmware against itself; this is
 * the one path that tries to ask the *panel* what it is holding, which is
 * where the reported stripes would show up: a band the firmware believes
 * is already on the panel and is not looks perfect in the framebuffer.
 *
 * The ST7789 has a memory-read command (0x2E, RAMRD) that clocks frame
 * memory back out of SDO. Measured on this hardware it does not work: the
 * X-NUCLEO-GFX01M2 leaves the panel's SDO unconnected, so PA6/MISO floats.
 * The probe below shows both halves of that — no byte alignment at any of
 * four SPI clocks ever matches a pattern written on purpose
 * (dbg_lcd_readback_probe stays 0) — and hal_miso_probe() settles it: with
 * the STM32's internal pull-up on PA6 the line reads high, with the
 * pull-down it reads low, so nothing is driving it. A whole-frame
 * comparison consequently reports tens of thousands of differing bytes
 * (dbg_lcd_readback_diff, which is read noise, not panel content) and
 * dbg_lcd_readback_ok stays 0.
 *
 * The path is kept compiled in because it is the right check on a board
 * where SDO *is* wired, and because it is what the measurement above was
 * made with. What stands in for it here is the referee plus the stress
 * test further up, which check the skip decision from the firmware side.
 *
 * It is a diagnostic, not part of the frame path: a full frame is 122,880
 * bytes in each direction (~50 ms of wire time), so it runs once shortly
 * after boot and after that only when dbg_lcd_readback_req is written over
 * SWD. dbg_lcd_readback_runs counts the runs, so a request that produced
 * no run means the firmware never got to it.
 */
extern volatile uint32_t dbg_frames;         /* main.c */

volatile uint32_t dbg_lcd_readback_req;      /* write 1 over SWD: run it    */
volatile uint32_t dbg_lcd_readback_ok;       /* 1 = panel == framebuffer    */
volatile uint32_t dbg_lcd_readback_runs;
volatile uint32_t dbg_lcd_readback_checks;   /* bytes compared              */
volatile uint32_t dbg_lcd_readback_diff;     /* bytes that differed         */
volatile uint32_t dbg_lcd_readback_bands_bad;
volatile uint32_t dbg_lcd_readback_first_bad;/* band of the first difference */
volatile uint32_t dbg_lcd_readback_want;     /* first differing byte pair   */
volatile uint32_t dbg_lcd_readback_got;
volatile uint32_t dbg_lcd_readback_probe;    /* bitmask: what answered, see
                                              * the probe below             */
volatile uint32_t dbg_lcd_readback_dummy;    /* leading dummy byte in use   */
volatile uint32_t dbg_lcd_readback_speed;    /* SPI clock that worked, kHz  */
volatile uint32_t dbg_lcd_readback_word;     /* first 4 bytes of a probe    */
volatile uint32_t dbg_lcd_readback_last;     /* frames when it last ran     */
volatile uint8_t  dbg_lcd_probe_raw[32];     /* raw bytes clocked out       */
volatile uint32_t dbg_lcd_probe_miso;        /* PA6 sampled with the panel
                                              * addressed, 1 bit per sample */

static uint32_t rb_dummy;        /* 1 = the panel sends one dummy byte first */
static uint32_t rb_baud;         /* CR1 BR value the probe worked at (0=40MHz) */
static int      rb_probed;       /* the probe (and the autostart) have run   */
static int      rb_autostart_done;   /* the automatic run happened */
#define LCD_READBACK_FRAME 30    /* one automatic run this many frames after boot */

/* Read `n` bytes of panel frame memory from the origin of the window.
 * After 0x2E the panel starts driving SDO; the master has to keep the
 * clock running, which spi_read() does (it sends 0xFF and captures MISO).
 * The caller ends the transfer with cs(1). */
static void panel_read_begin(uint16_t x0, uint16_t y0,
                             uint16_t x1, uint16_t y1)
{
    set_window_addr(x0, y0, x1, y1);
    cmd(0x2E);                             /* RAMRD */
    dc(1); cs(0);
    if (rb_dummy) { uint8_t d; spi_read(&d, 1); }
}

/* Does the panel answer at all, and with what byte order and clock?
 *
 * Writes four known pixels into the left black bar (0..3, 0 — outside the
 * NES picture, and put back to black right after) and clocks 32 bytes back
 * at four SPI clocks, looking for the pattern at both possible byte
 * alignments (the first byte after 0x2E may be a dummy — the ST7789
 * datasheet says it is). The result is a bitmask in dbg_lcd_readback_probe
 * (bit 2*clock + alignment), plus the raw bytes of the last attempt in
 * dbg_lcd_probe_raw for a post mortem over SWD.
 *
 * A panel that answers nothing at any clock returns whatever the floating
 * line happens to pick up, matches no alignment and leaves the mask at 0 —
 * which is what this shield does (see hal_miso_probe() for the proof that
 * the line is not driven at all). */
static uint32_t readback_probe(void)
{
    static const uint8_t src[4] = { 0x0F, 0x21, 0x30, 0x16 };
    static const uint32_t baud[4] = { 0, 2, 4, 7 };  /* 40, 10, 2.5, 0.31 MHz */
    static const uint32_t khz[4]  = { 40000, 10000, 2500, 312 };
    uint8_t pat[8], got[32];
    const uint32_t n = 4u * LCD_BPP / 8u;   /* 4 pixels: 8 bytes 16-bit,
                                             * 6 bytes 12-bit             */
    uint32_t mask = 0, first = 0;

    for (int c = 0; c < 4; c++) {
        spi_set_baud(baud[c]);
        spi_rx_flush();

        conv_pixels(src, pat, 4);
        set_window(0, 0, 3, 0);
        dc(1); cs(0); spi_write(pat, n); cs(1);

        panel_read_begin(0, 0, 3, 0);
        if (c == 0) dbg_lcd_probe_miso = hal_miso_probe(0);
        spi_read(got, sizeof(got));
        cs(1);

        for (uint32_t i = 0; i < sizeof(got); i++)
            dbg_lcd_probe_raw[i] = got[i];
        if (c == 0) {
            dbg_lcd_readback_word = (uint32_t)got[0] | ((uint32_t)got[1] << 8)
                                  | ((uint32_t)got[2] << 16)
                                  | ((uint32_t)got[3] << 24);
        }
        if (memcmp(got, pat, n) == 0) {
            mask |= 1u << (2 * c);          /* data starts immediately */
            if (!first) { first = 1; dbg_lcd_readback_speed = khz[c]; }
        }
        if (memcmp(got + 1, pat, n) == 0) {
            mask |= 1u << (2 * c + 1);      /* one dummy byte first    */
            if (!first) { first = 2; dbg_lcd_readback_speed = khz[c]; }
        }
    }

    /* the working combination, if any: prefer the fastest clock, and the
     * dummy byte only if the direct alignment never matched */
    if (mask & 1u)      { rb_baud = 0; rb_dummy = 0; }
    else if (mask & 2u) { rb_baud = 0; rb_dummy = 1; }
    else if (mask & 4u) { rb_baud = 2; rb_dummy = 0; }
    else if (mask & 8u) { rb_baud = 2; rb_dummy = 1; }
    else {
        rb_baud = 2;                        /* slowest sensible clock for
                                             * the full check if nothing
                                             * answered */
        rb_dummy = 0;
    }

    memset(pat, 0, n);                      /* put the bar back to black */
    spi_set_baud(0);
    set_window(0, 0, 3, 0);
    dc(1); cs(0); spi_write(pat, n); cs(1);
    return mask;
}

/* The panel's SDO does not have to keep up with the 40 MHz the picture is
 * clocked out at, so the probe tries the fast clock and then a quarter of
 * it, and the full check uses whatever worked. */
static void readback_probe_all(void)
{
    uint32_t mask = readback_probe();
    dbg_lcd_readback_probe = mask;
    dbg_lcd_readback_dummy = rb_dummy;
    dbg_lcd_readback_speed = (mask == 0) ? 0 : dbg_lcd_readback_speed;
    rb_probed = 1;
}

/* The whole picture: convert every band with the same conv_pixels() the
 * send path uses, read the same rectangle back out of the panel and
 * compare. Byte-wise, because the point is to say exactly how many bytes
 * differ and where the first one is. */
void lcd_readback_check(void)
{
    static uint8_t want[BAND_BYTES + 8];
    static uint8_t got[256];               /* the band is read in chunks */
    uint32_t diff = 0, checks = 0, bands_bad = 0, first_bad = 0xFFFFFFFFu;
    uint32_t want_byte = 0, got_byte = 0;

    if (!rb_probed) readback_probe_all();
    spi_set_baud(rb_baud);
    spi_rx_flush();

    for (int b = 0; b < LCD_H / BAND_H; b++) {
        int y0 = b * BAND_H;
        uint32_t d = 0;
        conv_pixels(fb + (uint32_t)y0 * FB_W, want, NES_W * BAND_H);
        panel_read_begin(NES_X, (uint16_t)y0,
                         (uint16_t)(NES_X + NES_W - 1),
                         (uint16_t)(y0 + BAND_H - 1));
        for (uint32_t off = 0; off < BAND_BYTES; off += sizeof(got)) {
            uint32_t n = BAND_BYTES - off;
            if (n > sizeof(got)) n = sizeof(got);
            spi_read(got, n);
            for (uint32_t i = 0; i < n; i++) {
                checks++;
                if (want[off + i] != got[i]) {
                    if (diff == 0) {
                        want_byte = want[off + i];
                        got_byte = got[i];
                    }
                    diff++;
                    d++;
                }
            }
        }
        cs(1);
        if (d) {
            bands_bad++;
            if (first_bad == 0xFFFFFFFFu) first_bad = (uint32_t)b;
        }
    }

    spi_set_baud(0);                        /* back to the picture clock */
    dbg_lcd_readback_checks = checks;
    dbg_lcd_readback_diff = diff;
    dbg_lcd_readback_bands_bad = bands_bad;
    dbg_lcd_readback_first_bad = first_bad;
    dbg_lcd_readback_want = want_byte;
    dbg_lcd_readback_got = got_byte;
    dbg_lcd_readback_last = dbg_frames;
    dbg_lcd_readback_runs++;
    dbg_lcd_readback_ok = (diff == 0);
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
    force_send = 1;                       /* no history: send it all */

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

    /* Ask the panel whether it can talk back at all (and which byte order
     * its RAMRD uses). Cheap — 20 bytes — and it makes the readback state
     * visible over SWD right from boot instead of only when a whole-frame
     * check is requested. */
    readback_probe_all();
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
    /* The referee costs a fingerprint of every band (dbg_cyc_diff shows it,
     * ~1.6 ms a frame), so it runs when it is needed and not otherwise:
     * through the boot window, whenever dbg_lcd_stress is running, and
     * whenever dbg_lcd_check_req is left set over SWD. When it comes on its
     * fingerprints have to be re-seeded from the framebuffer, which at a
     * frame boundary is what the panel holds. */
    int want = (check_bands > 0) || (dbg_lcd_check_req != 0)
               || (dbg_lcd_stress != 0);
    if (want && !referee) referee_seed();
    referee = want;

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
    const int band = y0 / BAND_H;

    uint32_t t = CYC_NOW();

    /* The decision: the 1 KB band against the 1 KB the panel holds, word
     * by word. */
    int differs = band_differs(src, held);
    int changed = differs || force_send;   /* no history: send it all */

    /* the referee: fingerprint the band we are about to judge */
    uint32_t fp = 0;
    if (referee)
        fp = band_fp(src);

    if (check_bands > 0) {
        /* Boot window: the same question the obvious way, over every byte,
         * and against the unforced answer (a forced frame says "changed"
         * whatever the bytes are). */
        uint32_t slow = 0;
        for (int i = 0; i < NES_W * BAND_H; i++)
            slow |= (uint32_t)(src[i] ^ held[i]);
        check_bands--;
        dbg_lcd_band_checked++;
        if ((slow == 0) == differs) dbg_lcd_band_ok = 0;
    }

    if (!changed) {
        acc_skipped++;
        if (referee) {
            dbg_lcd_invariant_checks++;
            if (sent_fp[band] != fp) {
                dbg_lcd_invariant_bad++;
                dbg_lcd_invariant_ok = 0;
            }
        }
        acc_diff += CYC_NOW() - t;
        return;                       /* the panel is already showing this */
    }

    /* the panel is about to hold exactly these bytes */
    memcpy(held, src, sizeof(held));
    if (referee)
        sent_fp[band] = fp;
    if (band == stress_watch)
        stress_seen = 1;              /* the injected band came back */

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
 * about 2 ms, of every frame.
 *
 * This is also where a band is snapshotted (see held[]): the row handed
 * out still holds what the panel holds, and the renderer is about to
 * overwrite it. */
uint8_t *lcd_nes_line_target(int y)
{
    uint8_t *row = fb + (uint32_t)y * FB_W;
    if ((y & (BAND_H - 1)) == 0 && !force_send) {
        uint32_t t = CYC_NOW();
        memcpy(held, row, sizeof(held));   /* 4 rows, one contiguous run */
        acc_copy += CYC_NOW() - t;
    }
    return row;
}

void lcd_nes_line(int y, const uint8_t *line)
{
    uint32_t t0 = CYC_NOW();
    uint8_t *dst = fb + (uint32_t)y * FB_W;
    dbg_lines = (uint32_t)y;

    /* a caller that did not render into the framebuffer row still gets the
     * picture (the host tools do, and nes_line_target may be unset); the
     * band snapshot then has to happen here, while the row in the
     * framebuffer is still the one the panel was given */
    if (line != dst) {
        if ((y & (BAND_H - 1)) == 0 && !force_send) {
            uint32_t t = CYC_NOW();
            memcpy(held, dst, sizeof(held));
            acc_copy += CYC_NOW() - t;
        }
        memcpy(dst, line, NES_W);
    }

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

    /* the first frame has sent everything: from now on a band is only sent
     * when it really changed. dbg_lcd_resync asks for one more full frame
     * (recovery, and proof that the panel is being refreshed at all). */
    if (force_send) { force_send = 0; dbg_lcd_forced++; }
    if (dbg_lcd_resync) { dbg_lcd_resync = 0; force_send = 1; }

    stress_tick();

    /* the panel readback: on request (dbg_lcd_readback_req over SWD), plus
     * one automatic run a moment after boot so that a fresh board reports
     * a result with nobody asking. Nothing is in flight here — the last
     * band has been drained above — so the SPI bus is free for it. */
    if (dbg_lcd_readback_req) {
        dbg_lcd_readback_req = 0;
        lcd_readback_check();
    } else if (!rb_autostart_done && dbg_frames >= LCD_READBACK_FRAME) {
        rb_autostart_done = 1;
        lcd_readback_check();
    }
}
