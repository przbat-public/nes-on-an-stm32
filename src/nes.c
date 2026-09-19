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
 * Mappers 0 (NROM), 1 (MMC1), 2 (UxROM) and 4 (MMC3) are implemented —
 * between them a large part of the library, from the early platformers to
 * the late bank-switching cartridges.
 */
#include "nes.h"
#include "cpu6502.h"
#include "ppu.h"
#include "nesmem.h"
#include "mapper.h"
#include "hal.h"

int nes_mapper;
int nes_prg_banks;
int nes_chr_banks;

void (*nes_line_hook)(int y, const uint8_t *line);
uint8_t *(*nes_line_target)(int y);

/* ----------------------------- cartridge -------------------------- */

uint8_t nes_ram_2k[0x800];         /* 2 KB console RAM (see nesmem.h) */
static uint8_t  wram[0x2000];      /* 8 KB cartridge work RAM         */
static uint8_t  chr_ram[0x2000];   /* used when the cart has CHR-RAM  */

const uint8_t *nes_prg[4] = { 0, 0, 0, 0 };
const uint8_t *nes_chr[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint8_t       *nes_chr_ram = 0;
int            nes_chr_is_ram = 0;

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
    dbg_pad_writes++;
    pad_strobe = (v & 1) != 0;
    if (pad_strobe)
        pad_shift = pad_state;
}

/* The last 64 $4016 reads as (cpu.pc << 16) | (strobe << 8) | bit, with
 * the live button state in the top byte: "the game never saw the button"
 * and "the game never asked for it" look identical from outside, and this
 * is what tells them apart (the CPU's pc is the address *during* the read,
 * so it is the instruction that did it). dbg_pad_reads/_writes count the
 * accesses, dbg_pad_bits keeps the last 32 bits shifted out. 260 bytes. */
volatile uint32_t dbg_pad_log[64];
volatile uint32_t dbg_pad_log_n;

static uint8_t pad_read(void)
{
    dbg_pad_reads++;
    if (pad_strobe) {
        uint8_t bit = (uint8_t)(pad_state & 1);
        dbg_pad_bits[dbg_pad_bit_n & 31] = bit;
        dbg_pad_bit_n++;
        dbg_pad_log[dbg_pad_log_n & 63] = ((uint32_t)cpu.pc << 16)
            | (1u << 8) | bit | ((uint32_t)pad_state << 24);
        dbg_pad_log_n++;
        return bit;
    }
    uint8_t bit = (uint8_t)(pad_shift & 1);
    dbg_pad_bits[dbg_pad_bit_n & 31] = bit;
    dbg_pad_bit_n++;
    pad_shift = (uint8_t)((pad_shift >> 1) | 0x80);
    dbg_pad_log[dbg_pad_log_n & 63] = ((uint32_t)cpu.pc << 16)
        | bit | ((uint32_t)pad_state << 24);
    dbg_pad_log_n++;
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
        return mapper_read(addr);       /* expansion (MMC5)       */
    dbg_ram_reads++;
    return wram[addr & 0x1FFF];         /* cartridge work RAM     */
}

void nes_bus_write_slow(uint16_t addr, uint8_t v)
{
    if (addr < 0x2000) {
        nes_ram_2k[addr & 0x7FF] = v;           /* fast path mirror */
    } else if (addr < 0x4000) {
        ppu_write_reg((uint16_t)(addr & 7), v);
        /* the MMC5 watches the two fully decoded registers only */
        if (addr == 0x2000 || addr == 0x2001)
            mapper_ppu_write(addr, v);
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
        mapper_write(addr, v);           /* expansion (MMC5) */
    } else if (addr < 0x8000) {
        /* cartridge work RAM: MMC5 gates this behind $5102/$5103 */
        dbg_ram_writes++;
        if (mapper_ram_writable())
            wram[addr & 0x1FFF] = v;
        else
            dbg_ram_drops++;
    } else {
        mapper_write(addr, v);                  /* mapper registers */
    }
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

    if (mapper != MAPPER_NROM && mapper != MAPPER_MMC1 &&
        mapper != MAPPER_UXROM && mapper != MAPPER_MMC3 &&
        mapper != MAPPER_MMC5)
        return NES_ERR_MAPPER;   /* NROM, MMC1, UxROM, MMC3 and MMC5 */

    uint32_t need = (uint32_t)(16 + trainer + prg_banks * 16384
                               + (chr_banks ? chr_banks * 8192 : 0));
    if (size < need)
        return NES_ERR_SIZE;

    const uint8_t *prg = rom + 16 + trainer;
    uint32_t prg_size = (uint32_t)(prg_banks * 16384);
    const uint8_t *chr = prg + prg_size;
    uint32_t chr_size = (uint32_t)(chr_banks * 8192);

    nes_chr_is_ram = (chr_banks == 0);
    nes_chr_ram = chr_ram;

    /* header mirroring: bit 0 clear = horizontal, set = vertical (MMC1
     * carts override this while the game runs) */
    ppu_mirroring = (flags6 & 1) ? 1 : 0;

    mapper_init(mapper, prg, prg_size, chr, chr_size);
    return NES_OK;
}

/* ------------------------------- frame ---------------------------- */

void nes_reset(void)
{
    for (int i = 0; i < 0x800; i++)  nes_ram_2k[i] = 0;
    for (int i = 0; i < 0x2000; i++) wram[i] = 0;
    for (int i = 0; i < 0x2000; i++) chr_ram[i] = 0;
    pad_state = pad_shift = 0;
    pad_strobe = false;

    ppu_chr_read  = chr_read_fn;
    ppu_chr_write = chr_write_fn;

    ppu_reset();
    mapper_reset();     /* MMC5 re-powers its register file, and for a
                         * CHR-ROM MMC5 cart installs its own $2007 CHR
                         * read (the bank depends on the last $512x write) */
    cpu_reset();
}

/* 262 scanlines (NTSC), 341 dots per line, 3 dots per CPU cycle:
 * the CPU budget for line y is the difference of 341*(y+1)/3 and
 * 341*y/3, which keeps the 2/3-cycle remainder exact. */
/* phase profiling: host cycles spent emulating the CPU, rendering the
 * PPU and converting bands (readable over SWD) */
volatile uint32_t dbg_irq_count, dbg_irq_line, dbg_nmi_count;
volatile uint32_t dbg_pad_reads, dbg_pad_writes;
volatile uint32_t dbg_ram_reads, dbg_ram_writes, dbg_ram_drops;
volatile uint8_t  dbg_pad_bits[32];
volatile uint32_t dbg_pad_bit_n;
volatile uint32_t dbg_cyc_cpu, dbg_cyc_ppu, dbg_cyc_frame;
/* the two pieces of the frame loop that had no counter before: the display
 * hook (which lcd.c times itself, see dbg_cyc_flush) and the per-line
 * bookkeeping around it (mapper tick, scroll reload, NMI) */
volatile uint32_t dbg_cyc_hook, dbg_cyc_loop;

/* lcd.c and ppu.c fill their own slice of the budget; publish theirs too,
 * at the frame boundary, so one SWD read is one whole frame */
void lcd_dbg_frame(void);
void ppu_dbg_frame(void);

#ifdef NES_PROFILING
#define CYC_NOW() cycles_now()      /* static inline in hal.h */
#else
#define CYC_NOW() 0u          /* host builds have no cycle counter */
#endif

void nes_run_frame(void)
{
    uint32_t t0 = CYC_NOW();
    uint32_t cpu_acc = 0, ppu_acc = 0, hook_acc = 0, loop_acc = 0;
    int prev = 0;
    for (int y = 0; y < 262; y++) {
        int now = 341 * (y + 1) / 3;
        uint32_t a = CYC_NOW();
        cpu_run(now - prev);
        uint32_t d = CYC_NOW();
        cpu_acc += d - a;
        prev = now;

        if (y == 241)
            ppu_set_vblank();
        if (y == 261) {
            ppu_clear_vblank();
            ppu_clear_sprite_flags(); /* sprite 0 / overflow restart */
            ppu_latch_scroll();      /* apply the scroll for this frame */
        }

        if (y < 240) {
            uint32_t c = CYC_NOW();
            /* the display layer hands us the row to render into: writing the
             * picture straight into its framebuffer saves copying all 240
             * scanlines again in the line hook */
            uint8_t *row = nes_line_target ? nes_line_target(y) : ppu_line;
            ppu_render_scanline(row, y);
            d = CYC_NOW();
            ppu_acc += d - c;
            if (nes_line_hook) {
                nes_line_hook(y, row);
                c = CYC_NOW();
                hook_acc += c - d;
                d = c;
            }

            /* one rendered line = one MMC3 (or MMC5) scanline-counter
             * tick; the interrupt is handed to the CPU before it runs the
             * next line, which is where a split-screen handler wants it */
            if (ppu_rendering_enabled()) {
                mapper_scanline(y);
                if (mapper_irq_pending() && cpu_irq()) {
                    dbg_irq_line = (uint32_t)y;   /* when the last one hit */
                    dbg_irq_count++;
                }
            }
        }
        ppu_end_scanline(y);

        if (ppu_nmi_pending()) {
            ppu_clear_nmi();
            cpu_nmi();
            dbg_nmi_count++;
        }
        loop_acc += CYC_NOW() - d;
    }
    ppu_frames++;
#ifdef NES_PROFILING
    lcd_dbg_frame();      /* host builds have no lcd.c to publish for */
#endif
    ppu_dbg_frame();
    dbg_cyc_cpu   = cpu_acc;
    dbg_cyc_ppu   = ppu_acc;
    dbg_cyc_hook  = hook_acc;
    dbg_cyc_loop  = loop_acc;
    dbg_cyc_frame = CYC_NOW() - t0;
}

uint8_t *nes_ram(void) { return nes_ram_2k; }

uint8_t *nes_prg_ram(uint32_t *size)
{
    if (size) *size = sizeof(wram);
    return wram;
}
