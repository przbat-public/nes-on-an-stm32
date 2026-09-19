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

/* display control pins (Arduino route on the GFX01M2 shield) */
#define PIN_CS  9    /* PA9  */
#define PIN_DC  10   /* PB10 */
#define PIN_RST 1    /* PA1  */

/* landscape orientation; if the picture comes out rotated 180 degrees
 * relative to the board, change this to 0xA0. */
#define MADCTL_LANDSCAPE 0x60

static uint8_t  fb[FB_W * LCD_H];      /* 256 x 240 indices */

/* diagnostics readable over SWD */
extern volatile uint32_t dbg_lines;
volatile uint32_t lcd_dbg_band_ok;
static uint16_t pal[64];

/* Band staging (4 KB): the CPU converts a band here, the DMA streams it
 * out while the PPU renders the next band. Only ONE transfer can be in
 * flight: SPI1_TX is wired to a single DMA channel, and starting a second
 * transfer would overwrite the first one's registers mid-flight. */
static uint8_t stage[BAND_BYTES];
static int     dma_inflight;

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

void lcd_init(void)
{
    uint8_t v;

    for (int i = 0; i < 64; i++)
        pal[i] = rgb565(NES_RGB[i][0], NES_RGB[i][1], NES_RGB[i][2]);

    for (uint32_t i = 0; i < sizeof(fb); i++) fb[i] = 0x0F;   /* black */

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

static void push_band(int y0)
{
    /* the previous band must be off the wire before we reuse the buffer
     * (it has had a whole band's worth of rendering time to finish) */
    while (dma_inflight > 0) {
        spi_dma_wait();
        dma_inflight--;
    }

    uint8_t *dst = stage;
    const uint8_t *src = fb + (uint32_t)y0 * FB_W;

    for (int i = 0; i < NES_W * BAND_H; i++) {
        uint16_t c = pal[src[i] & 0x3F];
        dst[0] = (uint8_t)(c >> 8);        /* ST7789 wants MSB first */
        dst[1] = (uint8_t)(c & 0xFF);
        dst += 2;
    }

    set_window(NES_X, (uint16_t)y0, (uint16_t)(NES_X + NES_W - 1),
               (uint16_t)(y0 + BAND_H - 1));
    dc(1); cs(0);
    if (spi_dma_available()) {
        spi_dma_start(stage, BAND_BYTES);
        dma_inflight = 1;
        lcd_dbg_band_ok = 1;
    } else {
        /* no DMA: shift the band out with the CPU */
        spi_write(stage, BAND_BYTES);
        cs(1);
    }
}

extern volatile uint32_t dbg_cyc_flush;

#ifdef NES_PROFILING
extern uint32_t cycles_now(void);
#define CYC_NOW() cycles_now()
#else
#define CYC_NOW() 0u
#endif

void lcd_nes_line(int y, const uint8_t *line)
{
    uint32_t t0 = CYC_NOW();
    uint8_t *dst = fb + (uint32_t)y * FB_W;
    dbg_lines = (uint32_t)y;
    for (int x = 0; x < NES_W; x++)
        dst[x] = line[x];

    if ((y & (BAND_H - 1)) == BAND_H - 1)
        push_band(y - (BAND_H - 1));

    dbg_cyc_flush += CYC_NOW() - t0;
}

void lcd_nes_frame_end(void)
{
    while (dma_inflight > 0) {
        spi_dma_wait();
        dma_inflight--;
    }
    cs(1);
}
