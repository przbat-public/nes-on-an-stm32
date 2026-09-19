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

    if (mapper_num == MAPPER_MMC3) {
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

/* ------------------------------- writes --------------------------- */

void mapper_write(uint16_t addr, uint8_t value)
{
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

/* ---------------------------- MMC3 IRQ ---------------------------- */

void mapper_scanline(void)
{
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

bool mapper_irq_pending(void) { return mmc3_irq_flag; }
