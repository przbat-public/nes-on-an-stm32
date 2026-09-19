/*
 * mapper.c — NROM and MMC1.
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
 */
#include "mapper.h"
#include "nesmem.h"
#include "ppu.h"

uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;

static int      mapper_num;
static const uint8_t *prg_base;
static uint32_t prg_banks;      /* in 16 KB units */
static const uint8_t *chr_base;
static uint32_t chr_banks;      /* in 8 KB units  */

static uint8_t shift_reg, shift_count;

/* publish the bank pointers the rest of the system reads */
static void apply_banks(void)
{
    if (mapper_num == MAPPER_MMC1) {
        uint32_t lo, hi;
        switch ((mmc1_control >> 2) & 3) {
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
        nes_prg_lo = prg_base + lo * 0x4000;
        nes_prg_hi = prg_base + hi * 0x4000;

        if (!nes_chr_is_ram && chr_banks) {
            if (mmc1_control & 0x10) {          /* two 4 KB banks */
                uint32_t b0 = (uint32_t)(mmc1_chr0 & 0x1F);
                uint32_t b1 = (uint32_t)(mmc1_chr1 & 0x1F);
                uint32_t max = chr_banks * 2;   /* in 4 KB units */
                if (b0 >= max) b0 = max ? max - 1 : 0;
                if (b1 >= max) b1 = max ? max - 1 : 0;
                nes_chr_lo = chr_base + b0 * 0x1000;
                nes_chr_hi = chr_base + b1 * 0x1000;
            } else {                            /* one 8 KB bank */
                uint32_t b = (uint32_t)(mmc1_chr0 & 0x1E) / 2;
                if (b >= chr_banks) b = chr_banks - 1;
                nes_chr_lo = chr_base + b * 0x2000;
                nes_chr_hi = nes_chr_lo + 0x1000;
            }
        }

        switch (mmc1_control & 3) {
        case 0:  ppu_mirroring = 2; break;      /* one-screen, lower */
        case 1:  ppu_mirroring = 3; break;      /* one-screen, upper */
        case 2:  ppu_mirroring = 1; break;      /* vertical          */
        default: ppu_mirroring = 0; break;      /* horizontal        */
        }
    } else {
        /* NROM: 16 KB carts mirror the single bank, 32 KB carts map
         * straight through */
        nes_prg_lo = prg_base;
        nes_prg_hi = (prg_banks == 1) ? prg_base : prg_base + 0x4000;
        if (!nes_chr_is_ram && chr_banks) {
            nes_chr_lo = chr_base;
            nes_chr_hi = chr_base + 0x1000;
        }
    }
}

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

    apply_banks();
}

void mapper_write(uint16_t addr, uint8_t value)
{
    if (mapper_num != MAPPER_MMC1)
        return;                 /* NROM ignores writes to the ROM area */

    if (value & 0x80) {         /* reset the serial port */
        shift_reg = 0;
        shift_count = 0;
        mmc1_control |= 0x0C;
        apply_banks();
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
    apply_banks();
}
