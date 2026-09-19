/*
 * mapper.c — NROM, MMC1, UxROM and MMC3.
 *
 * MMC1 details worth remembering:
 *   - writes to $8000-$FFFF are serial: five writes, bit 0 each time,
 *     LSB first; a write with bit 7 set resets the shift register (and
 *     on real hardware forces the control register into PRG mode 3);
 *   - the register that receives the completed five bits is chosen by
 *     the address: $8000 control, $A000 CHR bank 0, $C000 CHR bank 1,
 *     $E000 PRG bank;
 *   - the control register's low two bits select the nametable
 *     arrangement: 0/1 = one-screen lower/upper, 2 = vertical,
 *     3 = horizontal; bits 2-3 pick the PRG banking mode and bit 4 the
 *     CHR bank size.
 *
 * MMC3 is a register file rather than a serial port:
 *   - $8000 (even) bank select: bits 0-2 pick which register $8001
 *     writes, bit 6 swaps the PRG halves, bit 7 swaps the CHR halves;
 *   - $8001 (odd) the data for that register: R0/R1 are 2 KB CHR banks,
 *     R2-R5 are 1 KB CHR banks, R6/R7 are the two switchable 8 KB PRG
 *     banks;
 *   - $A000 mirroring, $A001 PRG RAM protection;
 *   - $C000 IRQ latch, $C001 reload, $E000 disable+acknowledge,
 *     $E001 enable.
 * Two of the four 8 KB PRG windows are always fixed: whichever pair the
 * mode bit does not swap points at the last two banks of the cartridge,
 * so the reset and interrupt vectors are always mapped.
 */
#include "mapper.h"
#include "nesmem.h"
#include "nes.h"        /* nes_prg_ram(): MMC5 banks the work RAM too */
#include "ppu.h"

uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;
uint8_t mmc3_select, mmc3_regs_dbg[8], mmc3_irq_latch_dbg;   /* diagnostics */

static int      mapper_num;
static const uint8_t *prg_base;
static uint32_t prg_banks;      /* in 16 KB units */
static const uint8_t *chr_base;
static uint32_t chr_banks;      /* in 8 KB units  */

static uint8_t shift_reg, shift_count;      /* MMC1 serial port */
static uint8_t uxrom_bank;                  /* UxROM: 16 KB bank at $8000 */

/* MMC3 state */
static uint8_t mmc3_regs[8];

/* diagnostics: how the game drives the MMC3 */
volatile uint32_t mmc3_wr_8000, mmc3_wr_8001, mmc3_wr_A000, mmc3_wr_C000,
                  mmc3_wr_C001, mmc3_wr_E000, mmc3_wr_E001;
static uint8_t mmc3_mirror_reg;
static uint8_t mmc3_irq_latch;
static uint8_t mmc3_irq_counter;
static bool    mmc3_irq_reload;
static bool    mmc3_irq_enabled;
static bool    mmc3_irq_flag;

/* ------------------------------- PRG ------------------------------ */

/* map four 8 KB banks into the four windows */
static void set_prg8(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    uint32_t banks = prg_banks * 2;             /* 8 KB units */
    uint32_t b[4] = { b0, b1, b2, b3 };

    if (banks == 0) banks = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t v = b[i] % banks;
        nes_prg[i] = prg_base + v * 0x2000;
    }
}

/* map one 16 KB bank into a pair of windows (what NROM and MMC1 need) */
static void set_prg16(int half, uint32_t bank)
{
    nes_prg[half * 2 + 0] = prg_base + bank * 0x4000;
    nes_prg[half * 2 + 1] = prg_base + bank * 0x4000 + 0x2000;
}

/* ------------------------------- CHR ------------------------------ */

static void set_chr1k(uint32_t bank0, uint32_t bank1, uint32_t bank2,
                      uint32_t bank3, uint32_t bank4, uint32_t bank5,
                      uint32_t bank6, uint32_t bank7)
{
    if (nes_chr_is_ram || chr_banks == 0)
        return;                                  /* CHR RAM: not banked */
    uint32_t banks = chr_banks * 8;              /* 1 KB units */
    uint32_t b[8] = { bank0, bank1, bank2, bank3,
                      bank4, bank5, bank6, bank7 };
    for (int i = 0; i < 8; i++)
        nes_chr[i] = chr_base + (b[i] % banks) * 0x400;
}

/* one 8 KB bank of CHR spread over the eight windows */
static void set_chr8k(uint32_t bank)
{
    uint32_t base = bank * 8;
    set_chr1k(base, base + 1, base + 2, base + 3,
              base + 4, base + 5, base + 6, base + 7);
}

/* two 4 KB banks (MMC1's 4 KB mode) */
static void set_chr4k(uint32_t bank0, uint32_t bank1)
{
    uint32_t a = bank0 * 4, b = bank1 * 4;
    set_chr1k(a, a + 1, a + 2, a + 3, b, b + 1, b + 2, b + 3);
}

/* UxROM: one switchable 16 KB bank at $8000, the last bank fixed at
 * $C000 so the reset and interrupt vectors never move. */
static void uxrom_apply(void)
{
    set_prg16(0, uxrom_bank % prg_banks);
    set_prg16(1, prg_banks - 1);
}

/* ---------------------------- mirroring --------------------------- */

/* 0 = horizontal, 1 = vertical, 2/3 = one-screen lower/upper (see ppu.c) */
static void mmc3_apply_mirror(uint8_t v)
{
    if (v & 0x80) ppu_mirroring = (v & 1) ? 3 : 2;
    else          ppu_mirroring = (v & 1) ? 0 : 1;
}

/* --------------------------- bank updates ------------------------- */

static void mmc1_apply(uint8_t control)
{
    uint32_t lo, hi;
    switch ((control >> 2) & 3) {
    case 0:                     /* 32 KB switch: low bank is even */
    case 1:
        lo = (uint32_t)(mmc1_prg & 0x0E);
        hi = lo + 1;
        break;
    case 2:                     /* first bank fixed at $8000 */
        lo = 0;
        hi = (uint32_t)(mmc1_prg & 0x0F);
        break;
    default:                    /* last bank fixed at $C000 */
        lo = (uint32_t)(mmc1_prg & 0x0F);
        hi = prg_banks - 1;
        break;
    }
    if (lo >= prg_banks) lo = prg_banks - 1;
    if (hi >= prg_banks) hi = prg_banks - 1;
    set_prg16(0, lo);
    set_prg16(1, hi);

    if (!nes_chr_is_ram && chr_banks) {
        if (control & 0x10) {                   /* two 4 KB banks */
            uint32_t b0 = (uint32_t)(mmc1_chr0 & 0x1F);
            uint32_t b1 = (uint32_t)(mmc1_chr1 & 0x1F);
            uint32_t max = chr_banks * 2;       /* in 4 KB units */
            if (b0 >= max) b0 = max - 1;
            if (b1 >= max) b1 = max - 1;
            set_chr4k(b0, b1);
        } else {                                /* one 8 KB bank */
            uint32_t b = (uint32_t)(mmc1_chr0 & 0x1E) / 2;
            if (b >= chr_banks) b = chr_banks - 1;
            set_chr8k(b);
        }
    }

    switch (control & 3) {
    case 0:  ppu_mirroring = 2; break;          /* one-screen, lower */
    case 1:  ppu_mirroring = 3; break;          /* one-screen, upper */
    case 2:  ppu_mirroring = 1; break;          /* vertical          */
    default: ppu_mirroring = 0; break;          /* horizontal        */
    }
}

static void mmc3_apply(void)
{
    uint32_t banks = prg_banks * 2;             /* 8 KB units */
    uint32_t last = banks ? banks - 1 : 0;
    uint32_t r6 = mmc3_regs[6], r7 = mmc3_regs[7];

    if (!(mmc3_select & 0x40))                  /* mode 0: $8000 swaps */
        set_prg8(r6, r7, last - 1, last);
    else                                        /* mode 1: $C000 swaps */
        set_prg8(last - 1, last, r6, r7);

    uint32_t r0 = mmc3_regs[0], r1 = mmc3_regs[1];
    if (!(mmc3_select & 0x80)) {                /* 2 KB at $0000/$0800 */
        set_chr1k((r0 & 0xFE), (r0 & 0xFE) + 1, (r1 & 0xFE), (r1 & 0xFE) + 1,
                  mmc3_regs[2], mmc3_regs[3], mmc3_regs[4], mmc3_regs[5]);
    } else {                                    /* 2 KB at $1000/$1800 */
        set_chr1k(mmc3_regs[2], mmc3_regs[3], mmc3_regs[4], mmc3_regs[5],
                  (r0 & 0xFE), (r0 & 0xFE) + 1, (r1 & 0xFE), (r1 & 0xFE) + 1);
    }
}

/* =============================== MMC5 ============================= */
/*
 * Mapper 5 is not a bank switcher with extras, it is a small chip with its
 * own memory. What lives here, in the order the code below is laid out:
 *
 *   PRG   $5100 mode 0-3. Mode 0 is one 32 KB bank from $5117; mode 1 is
 *         16 KB from $5115 at $8000 and 16 KB from $5117 at $C000; mode 2
 *         is 16 KB from $5115 plus 8 KB from $5116 and $5117; mode 3 is
 *         the four 8 KB banks $5114-$5117. Bits 6..1 (16 KB) or 6..0
 *         (8 KB) are the bank number; in a 16 KB register bit 0 is ignored
 *         and CPU A13 passes through. $5117 is ROM only and powers up at
 *         $FF, which is the last bank — that is where the vectors live.
 *         Bit 7 of $5114-$5116 maps the machine's 8 KB work RAM into that
 *         window instead of ROM, which is why the mapper needs a pointer
 *         to the work RAM rather than owning it.
 *   CHR   $5101 mode 0-3 (8/4/2/1 KB). $5120-$512B are ten-bit registers
 *         and the CHR mode decides how the written byte and the two bits
 *         of $5130 fill them; the fetch side reads them back with the
 *         same mode, which is what makes "the banks are always indexed by
 *         the currently selected size" work. $5120-$5127 bank sprites,
 *         $5128-$512B bank the background, and with 8x16 sprites on the
 *         background fetches really do use the second set (the MMC5 can
 *         tell the two apart). That is why ppu.c calls back for the
 *         sprite pass: the same PPU address means two different banks.
 *   NT    $5105 maps each of the four $2000-$2FFF pages onto CIRAM page 0,
 *         CIRAM page 1, the 1 KB of ExRAM or a synthesised fill page built
 *         from $5106/$5107. When the mapping is one of the four the PPU
 *         already implements (vertical, horizontal, one-screen 0/1) and
 *         nothing else needs substituting, the mapper hands the frame back
 *         to the PPU's own fast path; every other case goes through
 *         mmc5_bg_tile()/mmc5_nt_byte().
 *   ExRAM $5104 mode %01 turns each ExRAM byte into palette + 4 KB CHR
 *         bank for the tile at that nametable position, so the palette
 *         resolution becomes 8x8 pixels and the CHR bank changes per tile.
 *         Mode %00 makes ExRAM a fifth nametable. Modes %10/%11 take it
 *         away from the PPU and hand it to the CPU (read/write, read
 *         only). Modes %00/%01 only accept CPU writes while the picture is
 *         being drawn — that is the chip's actual rule, and it is why the
 *         CPU has to switch to %10 to fill ExRAM during vblank.
 *   split $5200-$5202 replace the nametable, the attributes and the CHR
 *         bank (a fixed 4 KB bank) over a run of tile columns, with their
 *         own vertical scroll in $5201. The tile data comes from ExRAM,
 *         which is being used as a nametable at the same time.
 *   IRQ   $5203/$5204, and $5205/$5206 are a multiplier the games use to
 *         compute addresses without a shift-and-add loop.
 *
 * Not implemented: the two pulse channels and the PCM channel
 * ($5000-$5015). This emulator has no APU at all, so there is nothing for
 * them to feed; writes are accepted and ignored.
 */

#define MMC5_EXRAM_SIZE 0x400

static uint8_t  mmc5_prg_mode;          /* $5100 */
static uint8_t  mmc5_chr_mode;          /* $5101 */
static uint8_t  mmc5_prot1, mmc5_prot2; /* $5102/$5103 */
static uint8_t  mmc5_exram_mode;        /* $5104 */
static uint8_t  mmc5_nt_map;            /* $5105 */
static uint8_t  mmc5_fill_tile;         /* $5106 */
static uint8_t  mmc5_fill_attr;         /* $5107, pre-spread to 0x55 steps */
static uint8_t  mmc5_ram_bank;          /* $5113 */
static uint8_t  mmc5_prg_reg[4];        /* $5114-$5117 */
static uint16_t mmc5_chr_reg[12];       /* $5120-$512B, ten bits each */
static uint8_t  mmc5_chr_upper;         /* $5130 */
static bool     mmc5_chr_io_bg;         /* last $512x write was $5128-$512B */
static uint8_t  mmc5_split;             /* $5200 */
static uint8_t  mmc5_split_scroll;      /* $5201 */
static uint8_t  mmc5_split_bank;        /* $5202 */
static uint8_t  mmc5_irq_target;        /* $5203 */
static bool     mmc5_irq_enable;        /* $5204 write, bit 7 */
static bool     mmc5_irq_pending;       /* $5204 read, bit 7 */
static uint16_t mmc5_scanline_no;
static uint16_t mmc5_mult;              /* $5205/$5206 */
static uint8_t  mmc5_mult_a, mmc5_mult_b;
static uint8_t  mmc5_ppu_ctrl;          /* the MMC5 watches $2000/$2001 */
static uint8_t  mmc5_ppu_mask;

static uint8_t  mmc5_exram[MMC5_EXRAM_SIZE];
static uint8_t *mmc5_ciram;             /* the PPU's 2 KB (ppu_ciram) */
static uint8_t *mmc5_ram;               /* the machine's 8 KB work RAM */
static uint32_t mmc5_ram_size;
static bool     mmc5_win_ram[4];        /* which $8000 windows map RAM */
static uint32_t mmc5_win_ram_off[4];    /* and where in the RAM chip */
static const uint8_t *mmc5_nt_page[4];  /* the four $2000-$2FFF pages;
                                         * NULL = the fill page */
static const uint8_t mmc5_zero_page[0x400];  /* $5104 %10/%11 nametables */

/* what nes_chr[] currently points at, so a line that does not need a
 * rebind does not pay for one */
enum { MMC5_CHR_NONE = 0, MMC5_CHR_BG, MMC5_CHR_SPR, MMC5_CHR_4K };
static uint8_t  mmc5_chr_state;
static uint32_t mmc5_chr_state_bank;

/* diagnostics — how much of the chip this cartridge actually drives */
volatile uint32_t mmc5_dbg_wr_chr_spr, mmc5_dbg_wr_chr_bg,
                  mmc5_dbg_wr_prg, mmc5_dbg_wr_chr, mmc5_dbg_wr_nt,
                  mmc5_dbg_wr_exram, mmc5_dbg_wr_irq, mmc5_dbg_wr_mul,
                  mmc5_dbg_wr_split, mmc5_dbg_wr_audio, mmc5_dbg_bg_hook,
                  mmc5_dbg_bg_extattr, mmc5_dbg_bg_split,
                  mmc5_dbg_exram_blank_drop, mmc5_dbg_irq,
                  mmc5_dbg_rd_5204, mmc5_dbg_rd_exram;

/* a ring of the last $5000-$5FFF writes (address << 8 | value), so the
 * host tool and a human can see what a cartridge actually drives */
volatile uint32_t mmc5_bank_hist[64];   /* which 16 KB banks $5115 saw */
volatile uint32_t mmc5_log[512];
volatile uint32_t mmc5_log_n;
volatile uint32_t mmc5_dbg_rd_ram, mmc5_dbg_wr_ram, mmc5_dbg_wr_prot;

/* ------------------------------ PRG ------------------------------- */

static void mmc5_prg_rom(int win, uint32_t bank8)
{
    uint32_t banks = prg_banks * 2;             /* 8 KB units */
    if (banks == 0) banks = 1;
    nes_prg[win] = prg_base + (bank8 % banks) * 0x2000;
    mmc5_win_ram[win] = false;
}

/* a window mapped to work RAM: the bank number is an 8 KB offset into the
 * chip, and the machine layer only has one 8 KB bank of it, so selecting
 * another one wraps (which is what a cartridge with less RAM installed
 * than the register allows looks like) */
static void mmc5_prg_ram(int win, uint32_t ram_bank8)
{
    uint32_t banks = mmc5_ram_size / 0x2000;
    if (banks == 0) banks = 1;
    uint32_t b = ram_bank8 % banks;
    nes_prg[win] = (const uint8_t *)(mmc5_ram + b * 0x2000);
    mmc5_win_ram[win] = true;
    mmc5_win_ram_off[win] = b * 0x2000;
}

/* one 16 KB window: bits 6..1 are the bank, bit 0 is ignored (CPU A13
 * comes straight from the bus), and bit 7 picks RAM over ROM on the
 * registers that have the toggle ($5114-$5116). Bit 7 is *not* part of
 * the bank number, so it has to be masked off before the shift — on a
 * 256 KB cartridge the wrap hides the mistake, on a bigger one it does
 * not. */
static void mmc5_prg16(int half, uint8_t reg, bool toggle)
{
    if (toggle && !(reg & 0x80)) {
        mmc5_prg_ram(half * 2 + 0, reg & 0x07);
        mmc5_prg_ram(half * 2 + 1, reg & 0x07);
    } else {
        uint32_t bank = (uint32_t)((reg & 0x7F) >> 1);
        mmc5_prg_rom(half * 2 + 0, bank * 2);
        mmc5_prg_rom(half * 2 + 1, bank * 2 + 1);
    }
}

static void mmc5_prg8(int win, uint8_t reg, bool toggle)
{
    if (toggle && !(reg & 0x80)) mmc5_prg_ram(win, reg & 0x07);
    else                         mmc5_prg_rom(win, (uint32_t)(reg & 0x7F));
}

static void mmc5_apply_prg(void)
{
    uint8_t r4 = mmc5_prg_reg[0], r5 = mmc5_prg_reg[1],
            r6 = mmc5_prg_reg[2], r7 = mmc5_prg_reg[3];

    switch (mmc5_prg_mode) {
    case 0: {                                   /* one 32 KB bank */
        uint32_t base = (uint32_t)((r7 & 0x7F) >> 2) * 4;
        for (int i = 0; i < 4; i++) mmc5_prg_rom(i, base + (uint32_t)i);
        break;
    }
    case 1:
        mmc5_prg16(0, r5, true);
        mmc5_prg16(1, r7, false);               /* $5117 is ROM only */
        break;
    case 2:
        mmc5_prg16(0, r5, true);
        mmc5_prg8(2, r6, true);
        mmc5_prg8(3, r7, false);
        break;
    default:
        mmc5_prg8(0, r4, true);
        mmc5_prg8(1, r5, true);
        mmc5_prg8(2, r6, true);
        mmc5_prg8(3, r7, false);
        break;
    }
}

/* ------------------------------ CHR ------------------------------- */

static uint32_t mmc5_chr_banks1k(void)
{
    uint32_t banks = chr_banks * 8;
    return banks ? banks : 1;
}

static const uint8_t *mmc5_chr_ptr(uint32_t bank1k)
{
    return chr_base + (bank1k % mmc5_chr_banks1k()) * 0x400;
}

/* where a PPU CHR address lands, for the sprite set, the background set,
 * or whichever of the two PPUDATA sees. This is the "CHR Ax" table from
 * the MMC5 documentation written out mode by mode. */
static uint32_t mmc5_chr_bank_for(uint16_t addr, bool bg)
{
    switch (mmc5_chr_mode) {
    case 3:                                     /* 1 KB */
        /* $5128-$512B cover the background's 4 KB window at either half
         * of the PPU's address space, so only two address bits index them */
        return mmc5_chr_reg[(bg ? 8 : 0) + ((addr >> 10) & (bg ? 3 : 7))];
    case 2: {                                   /* 2 KB */
        int idx = bg ? (9 + 2 * ((addr >> 11) & 1))
                     : (1 + 2 * ((addr >> 11) & 3));
        return (uint32_t)(mmc5_chr_reg[idx] >> 1) * 2 + ((addr >> 10) & 1);
    }
    case 1: {                                   /* 4 KB */
        /* the background's four registers cover both halves of the PPU
         * window; the sprite set has one register per half */
        int idx = bg ? 11 : (3 + 4 * ((addr >> 12) & 1));
        return (uint32_t)(mmc5_chr_reg[idx] >> 2) * 4 + ((addr >> 10) & 3);
    }
    default: {                                  /* 8 KB: $5127 only */
        int idx = bg ? 11 : 7;
        return (uint32_t)(mmc5_chr_reg[idx] >> 3) * 8 + ((addr >> 10) & 7);
    }
    }
}

/* Nine bits of CHR ROM is more than a uint16_t can address, so the PPU
 * gets 1 KB windows (nes_chr[]) and the mapper recomputes them. */
static void mmc5_bind_chr(int set)
{
    for (int j = 0; j < 8; j++)
        nes_chr[j] = mmc5_chr_ptr(mmc5_chr_bank_for((uint16_t)(j * 0x400),
                                                    set == MMC5_CHR_BG));
    mmc5_chr_state = (uint8_t)set;
}

static void mmc5_bind_chr4k(uint32_t bank4k)
{
    uint32_t banks = chr_banks * 2;             /* 4 KB units */
    uint32_t b = banks ? (bank4k % banks) : 0;
    for (int i = 0; i < 4; i++)
        nes_chr[i] = chr_base + b * 0x1000 + (uint32_t)i * 0x400;
    mmc5_chr_state = MMC5_CHR_4K;
    mmc5_chr_state_bank = bank4k;
}

/* 8x16 sprites are the case where the MMC5 really does keep two CHR
 * windows; with 8x8 sprites the background's registers are ignored and
 * both passes use $5120-$5127. Both need at least one rendering bit set
 * in $2001 (the MMC5 watches $2001 and drops the substitution when the
 * picture is off). */
static bool mmc5_spr16(void)
{
    return (mmc5_ppu_ctrl & 0x20) != 0 && (mmc5_ppu_mask & 0x18) != 0;
}

static void mmc5_use_bg_chr(void)
{
    int want = mmc5_spr16() ? MMC5_CHR_BG : MMC5_CHR_SPR;
    if (mmc5_chr_state != want) mmc5_bind_chr(want);
}

static void mmc5_use_spr_chr(void)
{
    if (mmc5_chr_state != MMC5_CHR_SPR) mmc5_bind_chr(MMC5_CHR_SPR);
}

static void mmc5_use_4k_chr(uint32_t bank4k)
{
    if (mmc5_chr_state == MMC5_CHR_4K && mmc5_chr_state_bank == bank4k)
        return;
    mmc5_bind_chr4k(bank4k);
}

/* writing a $512x register fills all ten internal bits, and which CPU data
 * bit goes where depends on the CHR mode in force at that moment ("the
 * 10-bit registers continue to store their value from the time they were
 * written") */
static void mmc5_chr_store(int i, uint8_t v)
{
    switch (mmc5_chr_mode) {
    case 3:
        mmc5_chr_reg[i] = (uint16_t)(((mmc5_chr_upper & 3) << 8) | v);
        break;
    case 2:
        mmc5_chr_reg[i] = (uint16_t)(((mmc5_chr_upper & 1) << 9)
                                     | ((v & 0xFF) << 1) | (v & 1));
        break;
    case 1:
        mmc5_chr_reg[i] = (uint16_t)(((v & 0xFF) << 2)
                                     | (mmc5_chr_reg[i] & 0x06) | (v & 1));
        break;
    default:
        mmc5_chr_reg[i] = (uint16_t)(((v & 0x7F) << 3)
                                     | (mmc5_chr_reg[i] & 0x06) | (v & 1));
        break;
    }
    mmc5_chr_io_bg = (i >= 8);
    mmc5_chr_state = MMC5_CHR_NONE;             /* rebind on next use */
}

/* $2007 reads of the pattern tables see the set that was written last */
static uint8_t mmc5_chr_ppu_read(uint16_t addr)
{
    uint32_t bank = mmc5_chr_bank_for(addr, mmc5_chr_io_bg);
    return mmc5_chr_ptr(bank)[addr & 0x3FF];
}

/* --------------------------- nametables --------------------------- */

static uint8_t mmc5_bg_tile(const ppu_bg_fetch_t *f, uint8_t *tile_out,
                            uint16_t *paddr_out);

static void mmc5_apply_nt(void)
{
    uint8_t m = mmc5_nt_map;

    for (int page = 0; page < 4; page++) {
        switch ((m >> (2 * page)) & 3) {
        case 0:  mmc5_nt_page[page] = mmc5_ciram;           break;
        case 1:  mmc5_nt_page[page] = mmc5_ciram + 0x400;   break;
        case 2:  /* ExRAM, which reads as all zeros while the CPU owns it */
            mmc5_nt_page[page] = (mmc5_exram_mode <= 1) ? mmc5_exram
                                                         : mmc5_zero_page;
            break;
        default: mmc5_nt_page[page] = 0;                    break; /* fill */
        }
    }

    /* Hand the frame back to the PPU's own nametable walk whenever the
     * mapping is one it already implements and nothing is being
     * substituted. That is the common case for a scrolling playfield, and
     * it keeps MMC5 cartridges from paying for the hook on every tile.
     *
     * $5105's names are the arrangement of the data, so they come out
     * crossed against the PPU's: MMC5 "vertical arrangement" ($50, NTA and
     * NTB on the same CIRAM page) is what the rest of this project calls
     * horizontal mirroring, and $44 is vertical.
     *
     * This has to clear ppu_bg_hook as well as set ppu_mirroring: the
     * renderer checks the hook first, so leaving it installed sends every
     * background tile through the mapper even when the mapping is one the
     * PPU's own walk reproduces byte for byte. Castlevania III spends the
     * whole game on $44/$50, so that mistake alone is what made the hook
     * rate 100% and the renderer three times its usual cost. */
    if (mmc5_exram_mode == 1 || (mmc5_split & 0x80) ||
        (m != 0x00 && m != 0x55 && m != 0x44 && m != 0x50)) {
        ppu_mirroring = PPU_MIRROR_MAPPER;
        ppu_bg_hook = mmc5_bg_tile;
    } else {
        ppu_bg_hook = 0;                        /* the PPU's own walk wins */
        switch (m) {
        case 0x00: ppu_mirroring = 2; break;    /* one-screen, CIRAM 0 */
        case 0x55: ppu_mirroring = 3; break;    /* one-screen, CIRAM 1 */
        case 0x44: ppu_mirroring = 1; break;    /* vertical   */
        default:   ppu_mirroring = 0; break;    /* horizontal */
        }
    }
}

static uint8_t mmc5_nt_byte(uint16_t addr)
{
    uint16_t off = (uint16_t)(addr & 0x3FF);
    const uint8_t *p = mmc5_nt_page[(addr >> 10) & 3];

    if (p) return p[off];
    /* fill mode: $5106 for tiles, $5107's two bits copied into all four
     * palette fields for the attribute table */
    return (off >= 0x3C0) ? mmc5_fill_attr : mmc5_fill_tile;
}

/* The PPU's own path, one nametable byte at a time: used by $2006/$2007
 * and by the renderer's tile fetch. */
static uint8_t mmc5_nt_read(uint16_t addr) { return mmc5_nt_byte(addr); }

static void mmc5_nt_write(uint16_t addr, uint8_t v)
{
    uint16_t off = (uint16_t)(addr & 0x3FF);
    uint8_t kind = (uint8_t)((mmc5_nt_map >> (2 * ((addr >> 10) & 3))) & 3);

    if (kind <= 1) {
        mmc5_ciram[(kind ? 0x400 : 0) + off] = v;
    } else if (kind == 2 && mmc5_exram_mode <= 1) {
        mmc5_exram[off] = v;
    }
    /* fill mode ignores writes; ExRAM in modes %10/%11 is not a nametable */
}

/* ------------------------ the PPU's tile hook --------------------- */

/*
 * One background tile column, with the cartridge owning everything about
 * it. Returns the palette and hands back the tile byte and the pattern
 * address; leaves nes_chr[] pointing at the bank the two pattern bytes
 * must come from, because all three substitution modes change it per
 * tile or per region.
 */
static uint8_t mmc5_bg_tile(const ppu_bg_fetch_t *f, uint8_t *tile_out,
                            uint16_t *paddr_out)
{
    uint8_t tile = mmc5_nt_byte(f->nt_addr);
    uint8_t palette;
    bool subs = (mmc5_ppu_mask & 0x18) != 0;

    mmc5_dbg_bg_hook++;

    /* ---- vertical split region: ExRAM is the nametable there ---- */
    if (subs && (mmc5_split & 0x80) && mmc5_exram_mode <= 1) {
        uint8_t thr = (uint8_t)(mmc5_split & 0x1F);
        bool left = (mmc5_split & 0x40) == 0;
        int col = f->col & 31;
        bool in_split = left ? (f->col < thr) : (f->col >= thr);

        if (in_split) {
            /* the split region has its own scanline counter, reset to
             * $5201 every vblank, and its own fine Y (CHR A0-A2, which is
             * what "CL mode" boards wire up) */
            uint16_t sc = (uint16_t)(mmc5_split_scroll + f->y);
            int srow = (sc >> 3) & 0x1F;
            int sfy = sc & 7;
            if (srow >= 30) srow -= 30;         /* vertical mirroring */
            uint16_t eoff = (uint16_t)(srow * 32 + col);
            uint8_t attr = mmc5_exram[0x3C0 | ((srow >> 2) << 3) | (col >> 2)];

            mmc5_dbg_bg_split++;
            mmc5_use_4k_chr(mmc5_split_bank);
            *tile_out = mmc5_exram[eoff & 0x3FF];
            *paddr_out = (uint16_t)(((*tile_out << 4) | sfy) & 0x0FFF);
            return (uint8_t)((attr >> (((srow & 2) << 1) | (col & 2))) & 3);
        }
        mmc5_use_bg_chr();
    }

    /* ---- extended attributes: ExRAM gives palette + 4 KB CHR bank ---- */
    if (subs && mmc5_exram_mode == 1) {
        uint8_t e = mmc5_exram[f->nt_addr & 0x3FF];
        mmc5_dbg_bg_extattr++;
        mmc5_use_4k_chr(((uint32_t)(mmc5_chr_upper & 3) << 6) | (e & 0x3F));
        *tile_out = tile;
        /* A4-A11 are the tile index, A10/A11 stay with the PPU; A12 and
         * up come from ExRAM + $5130, which is the 4 KB bank the mapper
         * just bound. */
        *paddr_out = (uint16_t)(((tile << 4) | f->fy) & 0x0FFF);
        return (uint8_t)(e >> 6);
    }

    /* ---- normal: the tile byte's page supplies the attribute byte -- */
    {
        /* Attribute address: one byte per 4x4 tile block, so the row term
         * is (coarse Y / 4) * 8 = (offset >> 7) << 3 = (offset >> 4) & 0x38.
         * Masking with 0x18 instead of 0x38 silently drops bit 5, which is
         * the whole term for coarse Y 16..23 and 24..29 — i.e. the palette
         * comes from the wrong row of the attribute table for the bottom
         * half of every nametable. The PPU's own walk computes it as
         * ((cy >> 2) << 3); the two must agree, and ppu_nt_test checks
         * exactly that. */
        uint16_t attr_addr = (uint16_t)(0x2000 | (f->nt_addr & 0xC00) | 0x3C0
                                        | ((f->nt_addr >> 4) & 0x38)
                                        | ((f->nt_addr >> 2) & 7));
        uint8_t attr = mmc5_nt_byte(attr_addr);
        palette = (uint8_t)((attr >> (((f->nt_addr >> 4) & 4)
                                      | (f->nt_addr & 2))) & 3);
    }
    mmc5_use_bg_chr();
    *tile_out = tile;
    *paddr_out = (uint16_t)(f->pat_base + tile * 16 + f->fy);
    return palette;
}

/* ------------------------- ExRAM, split, IRQ ---------------------- */

/* The CPU can only reach ExRAM while the PPU is drawing in modes %00/%01
 * (the chip's own rule — a game that wants to fill it during vblank must
 * switch to %10 first), read/write in %10, and read only in %11. */
static bool mmc5_exram_readable(void) { return mmc5_exram_mode >= 2; }

static bool mmc5_exram_writable(void)
{
    if (mmc5_exram_mode == 2) return true;
    if (mmc5_exram_mode == 3) return false;
    return ppu_visible();
}

static bool mmc5_ram_writable(void)
{
    return (mmc5_prot1 & 3) == 2 && (mmc5_prot2 & 3) == 1;
}

/* ------------------------------ writes ---------------------------- */

static void mmc5_write(uint16_t addr, uint8_t v)
{
    mmc5_log[mmc5_log_n++ & 511] = ((uint32_t)addr << 8) | v;
    switch (addr) {
    case 0x5000: case 0x5001: case 0x5002: case 0x5003:
    case 0x5004: case 0x5005: case 0x5006: case 0x5007:
    case 0x5010: case 0x5011: case 0x5015:
        mmc5_dbg_wr_audio++;
        return;                                  /* audio: no APU here */

    case 0x5100: mmc5_dbg_wr_prg++;
                 mmc5_prg_mode = (uint8_t)(v & 3); mmc5_apply_prg(); return;
    case 0x5101: mmc5_dbg_wr_chr++;
                 mmc5_chr_mode = (uint8_t)(v & 3);
                 mmc5_chr_state = MMC5_CHR_NONE; return;
    case 0x5102: mmc5_dbg_wr_prot++;
                 mmc5_prot1 = (uint8_t)(v & 3); return;
    case 0x5103: mmc5_dbg_wr_prot++;
                 mmc5_prot2 = (uint8_t)(v & 3); return;
    case 0x5104: mmc5_dbg_wr_nt++;
                 mmc5_exram_mode = (uint8_t)(v & 3);
                 mmc5_apply_nt(); return;
    case 0x5105: mmc5_dbg_wr_nt++;
                 mmc5_nt_map = v; mmc5_apply_nt(); return;
    case 0x5106: mmc5_fill_tile = v; return;
    case 0x5107: mmc5_fill_attr = (uint8_t)((v & 3) * 0x55); return;

    case 0x5113: mmc5_dbg_wr_prg++;
                 mmc5_ram_bank = (uint8_t)(v & 0x0F); return;
    case 0x5115: mmc5_bank_hist[(v >> 1) & 0x3F]++; /* fall through */
    case 0x5114: case 0x5116: case 0x5117:
        mmc5_dbg_wr_prg++;
        mmc5_prg_reg[addr - 0x5114] = v;
        mmc5_apply_prg();
        return;

    case 0x5130: mmc5_dbg_wr_chr++;
                 mmc5_chr_upper = (uint8_t)(v & 3);
                 mmc5_chr_state = MMC5_CHR_NONE;
                 return;

    case 0x5200: mmc5_dbg_wr_split++;
                 mmc5_split = v; mmc5_apply_nt(); return;
    case 0x5201: mmc5_dbg_wr_split++; mmc5_split_scroll = v; return;
    case 0x5202: mmc5_dbg_wr_split++; mmc5_split_bank = v; return;
    case 0x5203: mmc5_dbg_wr_irq++; mmc5_irq_target = v; return;
    case 0x5204: mmc5_dbg_wr_irq++;
                 mmc5_irq_enable = (v & 0x80) != 0; return;
    case 0x5205: mmc5_dbg_wr_mul++;
                 mmc5_mult_a = v;
                 mmc5_mult = (uint16_t)(mmc5_mult_a * mmc5_mult_b);
                 return;
    case 0x5206: mmc5_dbg_wr_mul++;
                 mmc5_mult_b = v;
                 mmc5_mult = (uint16_t)(mmc5_mult_a * mmc5_mult_b);
                 return;
    default: break;
    }

    if (addr >= 0x5120 && addr <= 0x512B) {
        mmc5_dbg_wr_chr++;
        if (addr >= 0x5128) mmc5_dbg_wr_chr_bg++;
        else                mmc5_dbg_wr_chr_spr++;
        mmc5_chr_store(addr - 0x5120, v);
        return;
    }
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        if (mmc5_exram_writable()) {
            mmc5_exram[addr & 0x3FF] = v;
            mmc5_dbg_wr_exram++;
        } else {
            /* modes %00/%01 outside the picture: the chip drops it */
            mmc5_dbg_exram_blank_drop++;
        }
        return;
    }
    /* a $5114-$5116 register can map work RAM into a PRG window; then a
     * write there is a RAM write and the ROM registers never see it */
    if (addr >= 0x8000) {
        int win = (addr >> 13) & 3;
        if (mmc5_win_ram[win] && mmc5_ram_writable())
            mmc5_ram[mmc5_win_ram_off[win] + (addr & 0x1FFF)] = v;
    }
}

static uint8_t mmc5_read(uint16_t addr)
{
    if (addr == 0x5204) {
        /* the "in frame" bit is PPU /RD activity, not a latch: it is set
         * while visible lines are being drawn and clear otherwise */
        uint8_t r = (uint8_t)((mmc5_irq_pending ? 0x80 : 0)
                              | (ppu_visible() ? 0x40 : 0));
        mmc5_dbg_rd_5204++;
        mmc5_irq_pending = false;                /* reading acknowledges */
        return r;
    }
    if (addr == 0x5205) return (uint8_t)(mmc5_mult & 0xFF);
    if (addr == 0x5206) return (uint8_t)(mmc5_mult >> 8);
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        mmc5_dbg_rd_exram++;
        return mmc5_exram_readable() ? mmc5_exram[addr & 0x3FF] : 0;
    }
    return 0;                                    /* open bus */
}

static void mmc5_reset(void)
{    mmc5_prg_mode = 3;              /* Koei's games rely on this default */
    mmc5_chr_mode = 3;
    mmc5_prot1 = 1;                 /* writes are locked out until the
                                     * game writes $02/$01 */
    mmc5_prot2 = 2;
    mmc5_exram_mode = 3;
    mmc5_nt_map = 0;
    mmc5_fill_tile = 0;
    mmc5_fill_attr = 0;
    mmc5_ram_bank = 0;
    for (int i = 0; i < 4; i++) mmc5_prg_reg[i] = 0;
    mmc5_prg_reg[3] = 0xFF;         /* $5117 powers up at $FF: last bank */
    for (int i = 0; i < 12; i++) mmc5_chr_reg[i] = 0;
    mmc5_chr_upper = 0;
    mmc5_chr_io_bg = false;
    mmc5_chr_state = MMC5_CHR_NONE;
    mmc5_split = 0;
    mmc5_split_scroll = 0;
    mmc5_split_bank = 0;
    mmc5_irq_target = 0;
    mmc5_irq_enable = false;
    mmc5_irq_pending = false;
    mmc5_scanline_no = 0;
    mmc5_mult = 0;
    mmc5_ppu_ctrl = 0;
    mmc5_ppu_mask = 0;
    for (int i = 0; i < MMC5_EXRAM_SIZE; i++) mmc5_exram[i] = 0;
    for (int i = 0; i < 4; i++) mmc5_win_ram[i] = false;
    mmc5_apply_prg();
    mmc5_apply_nt();

    /* The chip owns the nametables outright, so the PPU takes its mapper
     * path for this cartridge; and $2007 CHR reads go through the bank
     * registers rather than the eight windows, because which set they see
     * depends on which of $5120-$5127 / $5128-$512B was written last. */
    ppu_bg_hook = mmc5_bg_tile;
    ppu_chr_bg_hook = mmc5_use_bg_chr;
    ppu_chr_spr_hook = mmc5_use_spr_chr;
    ppu_nt_read_hook = mmc5_nt_read;
    ppu_nt_write_hook = mmc5_nt_write;
    if (!nes_chr_is_ram)
        ppu_chr_read = mmc5_chr_ppu_read;
}

/* ---------------------------- MMC5 IRQ ---------------------------- */

/* the register file as the game last left it, for tools/swd.py and the
 * host runner to read instead of guessing from the picture */
uint8_t mmc5_regs_dbg[16];
uint16_t mmc5_chr_dbg[12];

void mmc5_dbg_publish(void)
{
    mmc5_regs_dbg[0]  = mmc5_prg_mode;
    mmc5_regs_dbg[1]  = mmc5_chr_mode;
    mmc5_regs_dbg[2]  = mmc5_exram_mode;
    mmc5_regs_dbg[3]  = mmc5_nt_map;
    mmc5_regs_dbg[4]  = mmc5_split;
    mmc5_regs_dbg[5]  = mmc5_irq_target;
    mmc5_regs_dbg[6]  = mmc5_prg_reg[0];
    mmc5_regs_dbg[7]  = mmc5_prg_reg[1];
    mmc5_regs_dbg[8]  = mmc5_prg_reg[2];
    mmc5_regs_dbg[9]  = mmc5_prg_reg[3];
    mmc5_regs_dbg[10] = mmc5_chr_upper;
    mmc5_regs_dbg[11] = (uint8_t)((mmc5_irq_enable ? 1 : 0)
                                  | (mmc5_irq_pending ? 2 : 0)
                                  | (ppu_visible() ? 4 : 0));
    mmc5_regs_dbg[12] = mmc5_ppu_ctrl;
    mmc5_regs_dbg[13] = mmc5_ppu_mask;
    mmc5_regs_dbg[14] = (uint8_t)mmc5_mult;
    mmc5_regs_dbg[15] = (uint8_t)(mmc5_mult >> 8);
    for (int i = 0; i < 12; i++) mmc5_chr_dbg[i] = mmc5_chr_reg[i];
    mmc5_regs_dbg[11] = (uint8_t)(mmc5_regs_dbg[11]
                                  | (mmc5_chr_io_bg ? 8 : 0));
}

/* MMC5's scanline counter counts rendered lines within the frame and
 * compares against $5203 (value $00 never matches). The counter resets
 * when the PPU starts drawing, which in this renderer is the first
 * visible line the machine layer clocks. Clocking it with the line number
 * the machine layer hands us makes an IRQ land on the scanline the game
 * asked for; a rewrite of $5203 inside the handler can fire a second one
 * later in the same frame, which is what the chip does. */
static void mmc5_scanline(int y)
{
    if (y == 0) {
        mmc5_scanline_no = 0;
        return;
    }
    mmc5_scanline_no = (uint16_t)y;
    if (mmc5_irq_target != 0 && mmc5_scanline_no == mmc5_irq_target) {
        mmc5_irq_pending = true;
        mmc5_dbg_irq++;
    }
}

void mapper_scanline(int y)
{
    if (mapper_num == MAPPER_MMC5) {
        mmc5_dbg_publish();
        mmc5_scanline(y);
        return;
    }
    if (mapper_num != MAPPER_MMC3)
        return;

    if (mmc3_irq_counter == 0 || mmc3_irq_reload) {
        mmc3_irq_counter = mmc3_irq_latch;
        mmc3_irq_reload = false;
    } else {
        mmc3_irq_counter--;
    }

    if (mmc3_irq_counter == 0 && mmc3_irq_enabled)
        mmc3_irq_flag = true;
}

bool mapper_irq_pending(void)
{
    if (mapper_num == MAPPER_MMC5)
        return mmc5_irq_pending && mmc5_irq_enable;
    return mmc3_irq_flag;
}

/* ------------------------------- setup ---------------------------- */

void mapper_init(int number, const uint8_t *prg, uint32_t prg_size,
                 const uint8_t *chr, uint32_t chr_size)
{
    mapper_num = number;
    prg_base = prg;
    prg_banks = prg_size / 0x4000;
    if (prg_banks == 0) prg_banks = 1;
    chr_base = chr;
    chr_banks = chr_size / 0x2000;

    shift_reg = shift_count = 0;
    mmc1_control = 0x0C;        /* PRG mode 3: last bank fixed */
    mmc1_chr0 = mmc1_chr1 = mmc1_prg = 0;

    for (int i = 0; i < 8; i++) mmc3_regs[i] = 0;
    mmc3_select = 0;
    mmc3_irq_latch = mmc3_irq_counter = 0;
    mmc3_irq_reload = mmc3_irq_enabled = mmc3_irq_flag = false;

    uxrom_bank = 0;

    if (mapper_num == MAPPER_MMC5) {
        mmc5_ciram = ppu_ciram();
        mmc5_ram = nes_prg_ram(&mmc5_ram_size);
        mmc5_reset();
    } else if (mapper_num == MAPPER_MMC3) {
        mmc3_apply();           /* even the reset vectors need R6/R7 */
    } else if (mapper_num == MAPPER_UXROM) {
        if (!nes_chr_is_ram && chr_banks) set_chr8k(0);
        uxrom_apply();
    } else if (mapper_num == MAPPER_MMC1) {
        mmc1_apply(mmc1_control);
    } else {
        /* NROM: 16 KB carts mirror the single bank, 32 KB carts map
         * straight through */
        set_prg16(0, 0);
        set_prg16(1, (prg_banks == 1) ? 0 : 1);
        if (!nes_chr_is_ram && chr_banks)
            set_chr8k(0);
    }
}

/* ------------------------------- reads ---------------------------- */

uint8_t mapper_read(uint16_t addr)
{
    if (mapper_num == MAPPER_MMC5)
        return mmc5_read(addr);
    return 0;                   /* every other mapper: open bus */
}

bool mapper_ram_writable(void)
{
    if (mapper_num == MAPPER_MMC5)
        return mmc5_ram_writable();
    return true;                /* the other mappers' 8 KB is always on */
}

void mapper_reset(void)
{
    if (mapper_num == MAPPER_MMC5)
        mmc5_reset();
}

/* The MMC5 listens to the two PPU registers its substitutions depend on:
 * $2000's sprite size decides whether the background has its own CHR bank
 * set, and $2001's two rendering bits enable substitution at all. The
 * machine layer forwards exactly the decoded addresses. */
void mapper_ppu_write(uint16_t addr, uint8_t v)
{
    if (mapper_num != MAPPER_MMC5)
        return;
    if (addr == 0x2000) mmc5_ppu_ctrl = v;
    if (addr == 0x2001) {
        mmc5_ppu_mask = v;
    }
    mmc5_chr_state = MMC5_CHR_NONE;     /* the binding depends on both */
}

/* ------------------------------- writes --------------------------- */

void mapper_write(uint16_t addr, uint8_t value)
{
    if (mapper_num == MAPPER_MMC5) {
        mmc5_write(addr, value);        /* $5000-$5FFF and $8000-$FFFF */
        return;
    }

    if (addr < 0x8000)
        return;                 /* no other mapper has registers down here */

    if (mapper_num == MAPPER_MMC3) {
        bool odd = (addr & 1) != 0;

        if (addr < 0xA000) {                        /* $8000-$9FFF */
            if (!odd) {
                mmc3_wr_8000++;
                mmc3_select = value;
                mmc3_apply();
            } else {
                mmc3_wr_8001++;
                mmc3_regs[mmc3_select & 7] = value;
                mmc3_apply();
            }
        } else if (addr < 0xC000) {                 /* $A000-$BFFF */
            if (!odd) { mmc3_wr_A000++; mmc3_mirror_reg = value; mmc3_apply_mirror(value); }
            /* odd: PRG RAM protection — the 8 KB work RAM here is always
             * readable/writable, protection only silences writes */
        } else if (addr < 0xE000) {                 /* $C000-$DFFF */
            if (!odd) {
                mmc3_wr_C000++;
                mmc3_irq_latch = value;
            } else {
                mmc3_wr_C001++;
                mmc3_irq_counter = 0;
                mmc3_irq_reload = true;
            }
        } else {                                     /* $E000-$FFFF */
            if (!odd) {
                mmc3_wr_E000++;
                mmc3_irq_enabled = false;
                mmc3_irq_flag = false;               /* acknowledge */
            } else {
                mmc3_wr_E001++;
                mmc3_irq_enabled = true;
            }
        }

        mmc3_regs_dbg[0] = mmc3_regs[0];             /* readable over SWD */
        mmc3_regs_dbg[1] = mmc3_regs[1];
        mmc3_irq_latch_dbg = mmc3_irq_latch;
        return;
    }

    if (mapper_num == MAPPER_UXROM) {
        /* the low bits pick the bank; real boards AND the value with the
         * ROM byte they see (bus conflicts), which games avoid anyway */
        uxrom_bank = (uint8_t)(value & 0x0F);
        uxrom_apply();
        return;
    }

    if (mapper_num != MAPPER_MMC1)
        return;                 /* NROM ignores writes to the ROM area */

    if (value & 0x80) {         /* reset the serial port */
        shift_reg = 0;
        shift_count = 0;
        mmc1_control |= 0x0C;
        mmc1_apply(mmc1_control);
        return;
    }

    shift_reg |= (uint8_t)((value & 1) << shift_count);
    if (++shift_count < 5)
        return;

    switch ((addr >> 13) & 3) {
    case 0: mmc1_control = shift_reg; break;
    case 1: mmc1_chr0    = shift_reg; break;
    case 2: mmc1_chr1    = shift_reg; break;
    default: mmc1_prg    = shift_reg; break;
    }
    shift_reg = 0;
    shift_count = 0;
    mmc1_apply(mmc1_control);
}

