/*
 * nes.c — the machine layer.
 *
 *   bus_read()/bus_write()  the NES memory map, including the mirroring
 *                           quirks games rely on
 *   nes_load()              iNES image -> PRG/CHR pointers, mapper check
 *   nes_run_frame()         one frame: 262 scanlines, the CPU gets its
 *                           113.67 cycles per line, the PPU renders the
 *                           visible ones, NMI fires on vblank
 *
 * Only mapper 0 (NROM) is implemented so far — that already covers a lot
 * of early cartridges, including the original platformers.
 */
#include "nes.h"
#include "cpu6502.h"
#include "ppu.h"
#include "nesmem.h"

int nes_mapper;
int nes_prg_banks;
int nes_chr_banks;

void (*nes_line_hook)(int y, const uint8_t *line);

/* ----------------------------- cartridge -------------------------- */

uint8_t nes_ram_2k[0x800];         /* 2 KB console RAM (see nesmem.h) */
static uint8_t  wram[0x800];       /* 2 KB cartridge work RAM         */
                                   /* ($6000-$7FFF mirrored; NROM     */
                                   /*  games rarely use more)         */
static uint8_t  chr_ram[0x2000];   /* used when the cart has CHR-RAM  */

const uint8_t *nes_prg = 0;
uint32_t       nes_prg_mask = 0;
const uint8_t *nes_chr = 0;
uint8_t       *nes_chr_ram = 0;
int            nes_chr_is_ram = 0;
static uint32_t prg_size = 0;

/* the PPU fetches patterns through this pointer, set at reset */
static uint8_t chr_read_fn(uint16_t addr) { return chr_read(addr); }
static void    chr_write_fn(uint16_t addr, uint8_t v)
{
    if (nes_chr_is_ram) nes_chr_ram[addr & 0x1FFF] = v;
}

/* ------------------------------ input ----------------------------- */

static uint8_t pad_state;      /* live button state            */
static uint8_t pad_shift;      /* shifted out on $4016 reads   */
static bool    pad_strobe;

void nes_set_buttons(uint8_t mask) { pad_state = mask; }

static void pad_write(uint8_t v)
{
    pad_strobe = (v & 1) != 0;
    if (pad_strobe)
        pad_shift = pad_state;
}

static uint8_t pad_read(void)
{
    if (pad_strobe)
        return (uint8_t)(pad_state & 1);
    uint8_t bit = (uint8_t)(pad_shift & 1);
    pad_shift = (uint8_t)((pad_shift >> 1) | 0x80);
    return bit;
}

/* ------------------------------- bus ------------------------------ */

uint8_t nes_bus_read_slow(uint16_t addr)
{
    if (addr < 0x4000)
        return ppu_read_reg((uint16_t)(addr & 7));
    if (addr == 0x4016)
        return pad_read();
    if (addr == 0x4017)
        return 0;                       /* player 2 not connected */
    if (addr < 0x4020)
        return 0;                       /* APU / IO registers     */
    if (addr < 0x6000)
        return 0;                       /* expansion              */
    return wram[addr & 0x7FF];
}

void nes_bus_write_slow(uint16_t addr, uint8_t v)
{
    if (addr < 0x4000) {
        ppu_write_reg((uint16_t)(addr & 7), v);
    } else if (addr == 0x4014) {
        /* OAM DMA: copy a page of RAM into sprite memory */
        uint16_t base = (uint16_t)(v << 8);
        for (int i = 0; i < 256; i++)
            ppu_write_reg(4, nes_ram_2k[(base + i) & 0x7FF]);
        cpu_stall = 513;                 /* the CPU stalls during DMA */
    } else if (addr == 0x4016) {
        pad_write(v);
    } else if (addr < 0x4020) {
        /* APU registers: not emulated yet */
    } else if (addr < 0x6000) {
        /* expansion */
    } else if (addr < 0x8000) {
        wram[addr & 0x7FF] = v;
    }
    /* writes to PRG ROM are ignored */
}

/* ---------------------------- iNES loader ------------------------- */

int nes_load(const uint8_t *rom, uint32_t size)
{
    if (size < 16 || rom[0] != 'N' || rom[1] != 'E' || rom[2] != 'S'
        || rom[3] != 0x1A)
        return NES_ERR_FORMAT;

    int prg_banks = rom[4];
    int chr_banks = rom[5];
    int flags6 = rom[6];
    int flags7 = rom[7];
    int mapper = (flags6 >> 4) | (flags7 & 0xF0);
    int trainer = (flags6 & 4) ? 512 : 0;

    nes_mapper = mapper;
    nes_prg_banks = prg_banks;
    nes_chr_banks = chr_banks;

    if (mapper != 0)
        return NES_ERR_MAPPER;           /* only NROM for now */

    uint32_t need = (uint32_t)(16 + trainer + prg_banks * 16384
                               + (chr_banks ? chr_banks * 8192 : 0));
    if (size < need)
        return NES_ERR_SIZE;

    nes_prg = rom + 16 + trainer;
    prg_size = (uint32_t)(prg_banks * 16384);
    nes_prg_mask = (prg_banks == 1) ? 0x3FFF : 0x7FFF;

    if (chr_banks == 0) {
        nes_chr_is_ram = 1;
        nes_chr = 0;
        nes_chr_ram = chr_ram;
    } else {
        nes_chr_is_ram = 0;
        nes_chr = nes_prg + prg_size;
        nes_chr_ram = chr_ram;
    }

    /* mirroring: bit 0 clear = horizontal, set = vertical */
    ppu_mirroring = (flags6 & 1) ? 1 : 0;
    return NES_OK;
}

/* ------------------------------- frame ---------------------------- */

void nes_reset(void)
{
    for (int i = 0; i < 0x800; i++)  nes_ram_2k[i] = 0;
    for (int i = 0; i < 0x800; i++)  wram[i] = 0;
    for (int i = 0; i < 0x2000; i++) chr_ram[i] = 0;
    pad_state = pad_shift = 0;
    pad_strobe = false;

    ppu_chr_read  = chr_read_fn;
    ppu_chr_write = chr_write_fn;

    ppu_reset();
    cpu_reset();
}

/* 262 scanlines (NTSC), 341 dots per line, 3 dots per CPU cycle:
 * the CPU budget for line y is the difference of 341*(y+1)/3 and
 * 341*y/3, which keeps the 2/3-cycle remainder exact. */
/* phase profiling: host cycles spent emulating the CPU, rendering the
 * PPU and converting bands (readable over SWD) */
volatile uint32_t dbg_cyc_cpu, dbg_cyc_ppu, dbg_cyc_flush, dbg_cyc_frame;

#ifdef NES_PROFILING
extern uint32_t cycles_now(void);
#define CYC_NOW() cycles_now()
#else
#define CYC_NOW() 0u          /* host builds have no cycle counter */
#endif

void nes_run_frame(void)
{
    uint32_t t0 = CYC_NOW();
    uint32_t cpu_acc = 0, ppu_acc = 0;
    int prev = 0;
    for (int y = 0; y < 262; y++) {
        int now = 341 * (y + 1) / 3;
        uint32_t a = CYC_NOW();
        cpu_run(now - prev);
        uint32_t b = CYC_NOW();
        cpu_acc += b - a;
        prev = now;

        if (y == 241)
            ppu_set_vblank();
        if (y == 261) {
            ppu_clear_vblank();
            ppu_latch_scroll();      /* apply the scroll for this frame */
        }

        if (y < 240) {
            uint32_t c = CYC_NOW();
            ppu_render_scanline(y);
            uint32_t d = CYC_NOW();
            ppu_acc += d - c;
            if (nes_line_hook)
                nes_line_hook(y, ppu_line);
        }
        ppu_end_scanline(y);

        if (ppu_nmi_pending()) {
            ppu_clear_nmi();
            cpu_nmi();
        }
    }
    ppu_frames++;
    dbg_cyc_cpu   = cpu_acc;
    dbg_cyc_ppu   = ppu_acc;
    dbg_cyc_frame = CYC_NOW() - t0;
}

uint8_t *nes_ram(void) { return nes_ram_2k; }
