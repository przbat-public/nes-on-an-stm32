/*
 * mapper.c — the cartridge mappers: NROM (0), MMC1 (1), UxROM (2),
 * MMC3 (4) and MMC5 (5).
 *
 * What this module owns: the bank registers of the five chips, the four
 * PRG and eight CHR bank pointers every memory access goes through
 * (nesmem.h), the PPU hooks an MMC5 cartridge needs, and the diagnostic
 * counters the host tools and tools/swd.py read. It never touches memory
 * itself — it publishes pointers, and the inline bus in nesmem.h does the
 * reads.
 *
 * What it assumes about the layer below: the machine layer (nes.c) has
 * already parsed the cartridge and hands mapper_init() the PRG and CHR
 * base pointers and sizes; it forwards writes to $8000-$FFFF, reads from
 * $4020-$5FFF and the two decoded PPU registers the MMC5 watches, and it
 * answers the $6000-$7FFF question (mapper_ram_writable). The PPU (ppu.c)
 * owns CIRAM and gives ppu_mirroring its meaning; the mapper only chooses
 * the number, and for MMC5 installs the nametable hooks ppu.h declares.
 *
 * The quirks each chip forced on the code below:
 *
 * NROM     no registers at all. A 16 KB cartridge mirrors its single bank
 *          into both halves; a 32 KB one maps straight through.
 *
 * MMC1     writes to $8000-$FFFF are serial: five writes, bit 0 each
 *          time, LSB first. A write with bit 7 set resets the shift
 *          register wherever it had got to, and also sets the control
 *          register's PRG mode bits, so a reset must be honoured even
 *          mid-byte. The register that receives the completed five bits is
 *          chosen by the address: $8000 control, $A000 CHR bank 0, $C000
 *          CHR bank 1, $E000 PRG bank. The control register's low two bits
 *          select the nametable arrangement: 0/1 = one-screen
 *          lower/upper, 2 = vertical, 3 = horizontal; bits 2-3 pick the
 *          PRG banking mode and bit 4 the CHR bank size.
 *
 * UxROM    one 16 KB bank at $8000 selected by the written value; $C000
 *          stays on the last bank so the vectors never move. Real boards
 *          have bus conflicts (the value is ANDed with the ROM byte it
 *          overwrites), which games avoid by construction, so nothing here
 *          models them. CHR is usually 8 KB of RAM on these boards, which
 *          the machine layer owns and this module must not bank.
 *
 * MMC3     a register file rather than a serial port:
 *          - $8000 (even) bank select: bits 0-2 pick which register $8001
 *            writes, bit 6 swaps the PRG halves, bit 7 swaps the CHR
 *            halves;
 *          - $8001 (odd) the data for that register: R0/R1 are 2 KB CHR
 *            banks, R2-R5 are 1 KB CHR banks, R6/R7 are the two switchable
 *            8 KB PRG banks;
 *          - $A000 mirroring, $A001 PRG RAM protection;
 *          - $C000 IRQ latch, $C001 reload, $E000 disable+acknowledge,
 *            $E001 enable.
 *          Two of the four 8 KB PRG windows are always fixed: whichever
 *          pair the mode bit does not swap points at the last two banks of
 *          the cartridge, so the reset and interrupt vectors are always
 *          mapped.
 *
 * MMC5     a small chip with its own memory rather than a bank switcher;
 *          the long comment above the MMC5 section below is the map of it.
 */
#include "mapper.h"
#include "nesmem.h"
#include "nes.h"        /* nes_prg_ram(): MMC5 banks the work RAM too */
#include "ppu.h"

/* Bank geometry. The names are sizes, because every use of them is "bank
 * number times this", and the mapper's registers count in the units the
 * windows are cut in. */
#define PRG_8K_SIZE   0x2000u    /* one nes_prg[] window                  */
#define PRG_16K_SIZE  0x4000u    /* one bank as NROM, MMC1 and UxROM see  */
#define CHR_1K_SIZE   0x400u     /* one nes_chr[] window                  */
#define CHR_4K_SIZE   0x1000u
#define CHR_8K_SIZE   0x2000u    /* one CHR bank in the iNES header       */

/* How many smaller units go into a bigger one. */
#define PRG_8K_PER_16K  2
#define PRG_8K_PER_32K  4
#define CHR_1K_PER_4K   4
#define CHR_1K_PER_8K   8
#define CHR_4K_PER_8K   2

/* The window counts come from nesmem.h and are not free to change. */
#define PRG_WINDOWS   4
#define CHR_WINDOWS   8

/* ppu_mirroring values; ppu.h owns what they mean and the PPU implements
 * them. MMC5 adds PPU_MIRROR_MAPPER, which hands the frame to this file. */
enum {
    MIRROR_HORIZONTAL       = 0,
    MIRROR_VERTICAL         = 1,
    MIRROR_ONE_SCREEN_LOWER = 2,
    MIRROR_ONE_SCREEN_UPPER = 3
};

uint8_t mmc1_control, mmc1_chr0, mmc1_chr1, mmc1_prg;
uint8_t mmc3_select, mmc3_regs_dbg[8], mmc3_irq_latch_dbg;   /* diagnostics */

static int      mapper_num;
static const uint8_t *prg_base;
static uint32_t prg_banks;      /* in 16 KB units */
static const uint8_t *chr_base;
static uint32_t chr_banks;      /* in 8 KB units  */

static uint8_t shift_reg, shift_count;      /* MMC1 serial port */
static uint8_t uxrom_bank;                  /* UxROM: 16 KB bank at $8000 */

/* MMC3 state. mmc3_select is the $8000 register: which R register the next
 * $8001 write lands in, plus the two mode bits mmc3_apply() reads. */
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

/* Map four 8 KB banks into the four windows. The bank numbers are taken
 * modulo the cartridge size on purpose: games do ask for banks past the
 * end of a small ROM, and the address decoder of a real board wraps the
 * same way, so clamping instead would show a different tile than the
 * hardware does. */
static void set_prg8(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    uint32_t banks = prg_banks * PRG_8K_PER_16K;
    uint32_t b[PRG_WINDOWS] = { b0, b1, b2, b3 };

    if (banks == 0) banks = 1;      /* cannot happen: mapper_init() floors
                                     * prg_banks at 1; here so the modulo
                                     * below can never divide by zero */
    for (int i = 0; i < PRG_WINDOWS; i++) {
        uint32_t v = b[i] % banks;
        nes_prg[i] = prg_base + v * PRG_8K_SIZE;
    }
}

/* Map one 16 KB bank into a pair of windows. NROM and MMC1 need this
 * because their registers hold 16 KB bank numbers, while nes_prg[] is cut
 * into 8 KB windows to match MMC3's smallest switch. */
static void set_prg16(int half, uint32_t bank)
{
    nes_prg[half * 2 + 0] = prg_base + bank * PRG_16K_SIZE;
    nes_prg[half * 2 + 1] = prg_base + bank * PRG_16K_SIZE + PRG_8K_SIZE;
}

/* ------------------------------- CHR ------------------------------ */

/* Map eight 1 KB banks into the eight CHR windows. A CHR RAM cartridge has
 * nothing to bank: nes_chr_is_ram means the windows point into the
 * machine's 8 KB of RAM (nes.c), and writing a bank pointer over them
 * would take the pattern tables away from it. */
static void set_chr1k(uint32_t bank0, uint32_t bank1, uint32_t bank2,
                      uint32_t bank3, uint32_t bank4, uint32_t bank5,
                      uint32_t bank6, uint32_t bank7)
{
    if (nes_chr_is_ram || chr_banks == 0)
        return;                                  /* CHR RAM: not banked */
    uint32_t banks = chr_banks * CHR_1K_PER_8K;
    uint32_t b[CHR_WINDOWS] = { bank0, bank1, bank2, bank3,
                                bank4, bank5, bank6, bank7 };
    for (int i = 0; i < CHR_WINDOWS; i++)
        nes_chr[i] = chr_base + (b[i] % banks) * CHR_1K_SIZE;
}

/* one 8 KB bank of CHR spread over the eight windows: the MMC1's 8 KB
 * mode, and every cartridge with fixed CHR */
static void set_chr8k(uint32_t bank)
{
    uint32_t base = bank * CHR_1K_PER_8K;       /* in 1 KB units */
    set_chr1k(base, base + 1, base + 2, base + 3,
              base + 4, base + 5, base + 6, base + 7);
}

/* two 4 KB banks (MMC1's 4 KB mode): the first in windows 0-3, the second
 * in windows 4-7 */
static void set_chr4k(uint32_t bank0, uint32_t bank1)
{
    uint32_t a = bank0 * CHR_1K_PER_4K, b = bank1 * CHR_1K_PER_4K;
    set_chr1k(a, a + 1, a + 2, a + 3, b, b + 1, b + 2, b + 3);
}

/* The board decodes four bank lines; the written value's high nibble is
 * ignored. */
#define UXROM_BANK_MASK 0x0F

/* UxROM: one switchable 16 KB bank at $8000, the last bank fixed at
 * $C000 so the reset and interrupt vectors never move. The modulo is the
 * same wrap a game asking for a bank the cartridge does not have gets
 * from the board. */
static void uxrom_apply(void)
{
    set_prg16(0, uxrom_bank % prg_banks);
    set_prg16(1, prg_banks - 1);
}

/* ---------------------------- mirroring --------------------------- */

/* MMC3's $A000. Bit 7 picks between the two one-screen arrangements, bit 0
 * the CIRAM page (or, with bit 7 clear, between horizontal and vertical).
 * The PPU has no idea which chip wrote the number, so the four cases land
 * in ppu.h's vocabulary here and nowhere else. */
#define MMC3_MIRROR_ONE_SCREEN 0x80
#define MMC3_MIRROR_PAGE       0x01

static void mmc3_apply_mirror(uint8_t v)
{
    if (v & MMC3_MIRROR_ONE_SCREEN)
        ppu_mirroring = (v & MMC3_MIRROR_PAGE) ? MIRROR_ONE_SCREEN_UPPER
                                               : MIRROR_ONE_SCREEN_LOWER;
    else
        ppu_mirroring = (v & MMC3_MIRROR_PAGE) ? MIRROR_HORIZONTAL
                                               : MIRROR_VERTICAL;
}

/* --------------------------- bank updates ------------------------- */

/* MMC1's $8000 control register: bits 0-1 mirroring, bits 2-3 PRG mode,
 * bit 4 CHR bank size. */
#define MMC1_CTRL_MIRROR_MASK   0x03
#define MMC1_CTRL_PRG_MODE_MASK 0x0C
#define MMC1_CTRL_CHR_4K        0x10
#define MMC1_CTRL_PRG_SHIFT     2

/* $E000 holds a 16 KB bank number; in 32 KB mode bit 0 is ignored, which
 * is what makes the pair even. $A000/$C000 hold a 4 KB bank number, and in
 * 8 KB mode bit 0 of CHR bank 0 is ignored. */
#define MMC1_PRG_32K_MASK  0x0E
#define MMC1_PRG_16K_MASK  0x0F
#define MMC1_CHR_4K_MASK   0x1F
#define MMC1_CHR_8K_MASK   0x1E

/* The serial port takes five writes, and bit 7 of a write resets it. */
#define MMC1_SERIAL_BITS  5
#define MMC1_SERIAL_RESET 0x80

/* Which register the completed five bits land in, from the address: the
 * chip decodes A13-A14. */
enum { MMC1_REG_CONTROL = 0, MMC1_REG_CHR0, MMC1_REG_CHR1, MMC1_REG_PRG };

static void mmc1_apply_prg(uint8_t control)
{
    uint32_t lo, hi;

    switch ((control & MMC1_CTRL_PRG_MODE_MASK) >> MMC1_CTRL_PRG_SHIFT) {
    case 0:                     /* 32 KB switch: low bank is even */
    case 1:
        lo = (uint32_t)(mmc1_prg & MMC1_PRG_32K_MASK);
        hi = lo + 1;
        break;
    case 2:                     /* first bank fixed at $8000 */
        lo = 0;
        hi = (uint32_t)(mmc1_prg & MMC1_PRG_16K_MASK);
        break;
    default:                    /* last bank fixed at $C000 */
        lo = (uint32_t)(mmc1_prg & MMC1_PRG_16K_MASK);
        hi = prg_banks - 1;
        break;
    }
    /* A small cartridge can be handed a bank number it does not have (the
     * register is 4 bits wide, so a 2-bank game can ask for 15). Falling
     * back to the last bank matches what the board's decoder does and
     * keeps the fixed half of the pair fixed. */
    if (lo >= prg_banks) lo = prg_banks - 1;
    if (hi >= prg_banks) hi = prg_banks - 1;
    set_prg16(0, lo);
    set_prg16(1, hi);
}

static void mmc1_apply_chr(uint8_t control)
{
    if (nes_chr_is_ram || chr_banks == 0)
        return;                                 /* CHR RAM: not banked */

    if (control & MMC1_CTRL_CHR_4K) {           /* two 4 KB banks */
        uint32_t b0 = (uint32_t)(mmc1_chr0 & MMC1_CHR_4K_MASK);
        uint32_t b1 = (uint32_t)(mmc1_chr1 & MMC1_CHR_4K_MASK);
        uint32_t max = chr_banks * CHR_4K_PER_8K;   /* in 4 KB units */
        if (b0 >= max) b0 = max - 1;
        if (b1 >= max) b1 = max - 1;
        set_chr4k(b0, b1);
    } else {                                    /* one 8 KB bank */
        uint32_t b = (uint32_t)(mmc1_chr0 & MMC1_CHR_8K_MASK)
                     / CHR_4K_PER_8K;
        if (b >= chr_banks) b = chr_banks - 1;
        set_chr8k(b);
    }
}

/* Control bits 0-1: the two one-screen arrangements, then vertical and
 * horizontal. The names agree with ppu.h here; it is the MMC5's $5105 that
 * uses the same two words the other way round. */
static void mmc1_apply_mirror(uint8_t control)
{
    switch (control & MMC1_CTRL_MIRROR_MASK) {
    case 0:  ppu_mirroring = MIRROR_ONE_SCREEN_LOWER; break;
    case 1:  ppu_mirroring = MIRROR_ONE_SCREEN_UPPER; break;
    case 2:  ppu_mirroring = MIRROR_VERTICAL;         break;
    default: ppu_mirroring = MIRROR_HORIZONTAL;       break;
    }
}

/* One control write moves the two bank sets and the arrangement, and the
 * chip does all three at once. They do not interact, so the split into
 * three functions is only for reading; nothing else calls them. */
static void mmc1_apply(uint8_t control)
{
    mmc1_apply_prg(control);
    mmc1_apply_chr(control);
    mmc1_apply_mirror(control);
}

/* MMC3's $8000 select register and the eight registers it addresses. */
#define MMC3_SELECT_REG_MASK 0x07   /* bits 0-2: which R register */
#define MMC3_SELECT_PRG_MODE 0x40   /* bit 6: swap the PRG halves */
#define MMC3_SELECT_CHR_MODE 0x80   /* bit 7: swap the CHR halves */

/* R0/R1 hold 2 KB CHR bank numbers, so their low bit is ignored. */
#define MMC3_CHR_2K_MASK 0xFE

/* The register file by the documentation's names: R0/R1 are the two 2 KB
 * CHR banks, R2-R5 the four 1 KB CHR banks, R6/R7 the two switchable 8 KB
 * PRG banks. */
enum {
    MMC3_R_CHR_2K_0 = 0, MMC3_R_CHR_2K_1,
    MMC3_R_CHR_1K_0, MMC3_R_CHR_1K_1, MMC3_R_CHR_1K_2, MMC3_R_CHR_1K_3,
    MMC3_R_PRG_0, MMC3_R_PRG_1
};

/* Both bank sets depend on the select register's two mode bits, so every
 * write to either register re-runs this. */
static void mmc3_apply(void)
{
    uint32_t banks = prg_banks * PRG_8K_PER_16K;
    uint32_t last = banks ? banks - 1 : 0;
    uint32_t r6 = mmc3_regs[MMC3_R_PRG_0], r7 = mmc3_regs[MMC3_R_PRG_1];

    if (!(mmc3_select & MMC3_SELECT_PRG_MODE))  /* mode 0: $8000 swaps */
        set_prg8(r6, r7, last - 1, last);
    else                                        /* mode 1: $C000 swaps */
        set_prg8(last - 1, last, r6, r7);

    uint32_t r0 = mmc3_regs[MMC3_R_CHR_2K_0], r1 = mmc3_regs[MMC3_R_CHR_2K_1];
    if (!(mmc3_select & MMC3_SELECT_CHR_MODE)) {  /* 2 KB at $0000/$0800 */
        set_chr1k((r0 & MMC3_CHR_2K_MASK), (r0 & MMC3_CHR_2K_MASK) + 1,
                  (r1 & MMC3_CHR_2K_MASK), (r1 & MMC3_CHR_2K_MASK) + 1,
                  mmc3_regs[MMC3_R_CHR_1K_0], mmc3_regs[MMC3_R_CHR_1K_1],
                  mmc3_regs[MMC3_R_CHR_1K_2], mmc3_regs[MMC3_R_CHR_1K_3]);
    } else {                                      /* 2 KB at $1000/$1800 */
        set_chr1k(mmc3_regs[MMC3_R_CHR_1K_0], mmc3_regs[MMC3_R_CHR_1K_1],
                  mmc3_regs[MMC3_R_CHR_1K_2], mmc3_regs[MMC3_R_CHR_1K_3],
                  (r0 & MMC3_CHR_2K_MASK), (r0 & MMC3_CHR_2K_MASK) + 1,
                  (r1 & MMC3_CHR_2K_MASK), (r1 & MMC3_CHR_2K_MASK) + 1);
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

#define MMC5_EXRAM_SIZE      0x400
#define MMC5_LOG_SIZE        512   /* writes kept for the host tool */
#define MMC5_BANK_HIST_SIZE  64    /* 16 KB banks $5115 has been asked for */
#define MMC5_CHR_REGS        12    /* $5120-$512B */
#define MMC5_PRG_REGS        4     /* $5114-$5117 */

/* $5100/$5101/$5104 and $5130 hold two-bit fields. */
#define MMC5_MODE_MASK 0x03

/* A palette field is two bits wherever one is extracted from an attribute
 * byte or an extended-attribute byte. */
#define MMC5_PALETTE_MASK 0x03

/* $5102/$5103 write-protection values. */
#define MMC5_PROT_MASK 0x03

/* $5107 holds two bits; copying them into all four palette fields of an
 * attribute byte is what 0x55 is. */
#define MMC5_FILL_ATTR_MASK   0x03
#define MMC5_FILL_ATTR_SPREAD 0x55

/* $5113: the work RAM bank, three bits (8 KB each). */
#define MMC5_RAM_BANK_MASK 0x0F

/* $5114-$5117: bit 7 maps work RAM into the window instead of ROM (only
 * $5114-$5116 have the bit), bits 6..1 a 16 KB bank, bits 6..0 an 8 KB
 * one. Only one 8 KB bank of work RAM exists, so the three bits the RAM
 * path uses wrap. */
#define MMC5_PRG_RAM_TOGGLE     0x80
#define MMC5_PRG_BANK_MASK      0x7F
#define MMC5_PRG_RAM_BANK_MASK  0x07

/* $5200 split-screen control: bit 7 on, bit 6 puts the region on the
 * right, bits 0-4 are the tile column it starts (or ends) at. */
#define MMC5_SPLIT_ENABLE    0x80
#define MMC5_SPLIT_RIGHT     0x40
#define MMC5_SPLIT_COL_MASK  0x1F

/* $5204 status: bit 7 the IRQ flag, bit 6 "the PPU is drawing now". */
#define MMC5_STATUS_IRQ      0x80
#define MMC5_STATUS_IN_FRAME 0x40

/* The nametable is 30 rows of 32 tiles; the attribute table sits in the
 * last 64 bytes of each 1 KB page, one byte per 4x4 tile block. */
#define NT_BASE        0x2000
#define NT_PAGE_SIZE   0x400
#define NT_PAGES       4       /* $2000-$2FFF, one page each */
#define NT_PAGE_MASK   0x0C00  /* A10-A11: which of the four pages */
#define NT_COLUMNS     32
#define NT_ROWS        30
#define NT_COARSE_MASK 0x1F    /* coarse X and Y are five bits */
#define NT_ATTR_OFFSET 0x3C0
#define NT_ATTR_ROW_MASK 0x38  /* (coarse Y >> 2) << 3 */
#define NT_ATTR_COL_MASK 0x07  /* coarse X >> 2 */

/* A pattern-table tile is 16 bytes (two 8-byte bitplanes) and a PPU
 * pattern address is twelve bits wide; ppu.c owns both numbers. */
#define TILE_BYTES         16
#define PATTERN_ADDR_MASK  0x0FFF

/* ExRAM's extended-attribute byte: six low bits of 4 KB CHR bank, two
 * high bits of palette. */
#define MMC5_EXTATTR_BANK_MASK 0x3F

/* $2000 bit 5 and $2001 bits 3-4, as ppu.c decodes them: sprite size, and
 * "the picture is being rendered". */
#define PPU_CTRL_SPRITE_16 0x20
#define PPU_MASK_RENDER    0x18

/* The four $5105 values whose mapping is one the PPU implements itself.
 * The two mixed ones are named after the arrangement the PPU ends up with,
 * which is crossed against $5105's own vocabulary: $50 is the MMC5's
 * "vertical arrangement" (NTA and NTB on the same CIRAM page) and is
 * horizontal here, and $44 is the other way round. */
#define MMC5_NT_MAP_ALL_CIRAM0 0x00
#define MMC5_NT_MAP_ALL_CIRAM1 0x55
#define MMC5_NT_MAP_HORIZONTAL 0x50
#define MMC5_NT_MAP_VERTICAL   0x44

static uint8_t  mmc5_prg_mode;          /* $5100 */
static uint8_t  mmc5_chr_mode;          /* $5101 */
static uint8_t  mmc5_prot1, mmc5_prot2; /* $5102/$5103 */
static uint8_t  mmc5_exram_mode;        /* $5104 */
static uint8_t  mmc5_nt_map;            /* $5105 */
static uint8_t  mmc5_fill_tile;         /* $5106 */
static uint8_t  mmc5_fill_attr;         /* $5107, pre-spread to 0x55 steps */
static uint8_t  mmc5_ram_bank;          /* $5113: the work RAM is a single
                                         * 8 KB bank here, so every value
                                         * points at the same memory and
                                         * nothing reads this back */
static uint8_t  mmc5_prg_reg[MMC5_PRG_REGS];    /* $5114-$5117 */
static uint16_t mmc5_chr_reg[MMC5_CHR_REGS];    /* $5120-$512B, ten bits */
static uint8_t  mmc5_chr_upper;         /* $5130 */
static bool     mmc5_chr_io_bg;         /* last $512x write was $5128-$512B */
static uint8_t  mmc5_split;             /* $5200 */
static uint8_t  mmc5_split_scroll;      /* $5201 */
static uint8_t  mmc5_split_bank;        /* $5202 */
static uint8_t  mmc5_irq_target;        /* $5203 */
static bool     mmc5_irq_enable;        /* $5204 write, bit 7 */
static bool     mmc5_irq_pending;       /* $5204 read, bit 7 */
static uint16_t mmc5_scanline_no;       /* what the counter compares */
static uint16_t mmc5_mult;              /* $5205/$5206 */
static uint8_t  mmc5_mult_a, mmc5_mult_b;
static uint8_t  mmc5_ppu_ctrl;          /* the MMC5 watches $2000/$2001 */
static uint8_t  mmc5_ppu_mask;

static uint8_t  mmc5_exram[MMC5_EXRAM_SIZE];
static uint8_t *mmc5_ciram;             /* the PPU's 2 KB (ppu_ciram) */
static uint8_t *mmc5_ram;               /* the machine's 8 KB work RAM */
static uint32_t mmc5_ram_size;
static bool     mmc5_win_ram[PRG_WINDOWS];       /* which $8000 windows
                                                  * map RAM */
static uint32_t mmc5_win_ram_off[PRG_WINDOWS];   /* and where in the RAM */
static const uint8_t *mmc5_nt_page[NT_PAGES];    /* the four $2000-$2FFF
                                                  * pages; NULL = the fill
                                                  * page */
/* A nametable the CPU has taken away from the PPU has to read as zeros:
 * pointing the page at NULL would send the renderer into the fill path and
 * paint $5106/$5107 instead. */
static const uint8_t mmc5_zero_page[MMC5_EXRAM_SIZE];

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
volatile uint32_t mmc5_bank_hist[MMC5_BANK_HIST_SIZE];
volatile uint32_t mmc5_log[MMC5_LOG_SIZE];
volatile uint32_t mmc5_log_n;
volatile uint32_t mmc5_dbg_rd_ram, mmc5_dbg_wr_ram, mmc5_dbg_wr_prot;

/* ------------------------------ PRG ------------------------------- */

static void mmc5_prg_rom(int win, uint32_t bank8)
{
    uint32_t banks = prg_banks * PRG_8K_PER_16K;
    if (banks == 0) banks = 1;
    nes_prg[win] = prg_base + (bank8 % banks) * PRG_8K_SIZE;
    mmc5_win_ram[win] = false;
}

/* A window mapped to work RAM: the bank number is an 8 KB offset into the
 * chip, and the machine layer only has one 8 KB bank of it, so selecting
 * another one wraps — which is what a cartridge with less RAM installed
 * than the register allows looks like. mmc5_win_ram_off is what makes the
 * write path below land in the same place the read path points at. */
static void mmc5_prg_ram(int win, uint32_t ram_bank8)
{
    uint32_t banks = mmc5_ram_size / PRG_8K_SIZE;
    if (banks == 0) banks = 1;
    uint32_t b = ram_bank8 % banks;
    nes_prg[win] = (const uint8_t *)(mmc5_ram + b * PRG_8K_SIZE);
    mmc5_win_ram[win] = true;
    mmc5_win_ram_off[win] = b * PRG_8K_SIZE;
}

/* One 16 KB window: bits 6..1 are the bank, bit 0 is ignored (CPU A13
 * comes straight from the bus), and bit 7 picks RAM over ROM on the
 * registers that have the toggle ($5114-$5116; $5117 is ROM only, which
 * is why its callers pass toggle = false). Bit 7 is *not* part of the bank
 * number, so it has to be masked off before the shift — on a 256 KB
 * cartridge the wrap hides the mistake, on a bigger one it does not. */
static void mmc5_prg16(int half, uint8_t reg, bool toggle)
{
    if (toggle && !(reg & MMC5_PRG_RAM_TOGGLE)) {
        mmc5_prg_ram(half * 2 + 0, reg & MMC5_PRG_RAM_BANK_MASK);
        mmc5_prg_ram(half * 2 + 1, reg & MMC5_PRG_RAM_BANK_MASK);
    } else {
        uint32_t bank = (uint32_t)((reg & MMC5_PRG_BANK_MASK) >> 1);
        mmc5_prg_rom(half * 2 + 0, bank * PRG_8K_PER_16K);
        mmc5_prg_rom(half * 2 + 1, bank * PRG_8K_PER_16K + 1);
    }
}

/* The 8 KB form of the same register: the whole seven bits are the bank. */
static void mmc5_prg8(int win, uint8_t reg, bool toggle)
{
    if (toggle && !(reg & MMC5_PRG_RAM_TOGGLE))
        mmc5_prg_ram(win, reg & MMC5_PRG_RAM_BANK_MASK);
    else
        mmc5_prg_rom(win, (uint32_t)(reg & MMC5_PRG_BANK_MASK));
}

/* $5100, in the documentation's numbering */
enum {
    MMC5_PRG_32K     = 0,   /* one 32 KB bank from $5117           */
    MMC5_PRG_16K     = 1,   /* 16 KB from $5115, 16 KB from $5117  */
    MMC5_PRG_16K_8K  = 2,   /* those, plus 8 KB from $5116         */
    MMC5_PRG_8K      = 3    /* the four 8 KB registers             */
};

static void mmc5_apply_prg(void)
{
    uint8_t r4 = mmc5_prg_reg[0], r5 = mmc5_prg_reg[1],
            r6 = mmc5_prg_reg[2], r7 = mmc5_prg_reg[3];

    switch (mmc5_prg_mode) {
    case MMC5_PRG_32K: {                        /* one 32 KB bank */
        uint32_t base = (uint32_t)((r7 & MMC5_PRG_BANK_MASK) >> 2)
                        * PRG_8K_PER_32K;
        for (int i = 0; i < PRG_WINDOWS; i++)
            mmc5_prg_rom(i, base + (uint32_t)i);
        break;
    }
    case MMC5_PRG_16K:
        mmc5_prg16(0, r5, true);
        mmc5_prg16(1, r7, false);               /* $5117 is ROM only */
        break;
    case MMC5_PRG_16K_8K:
        mmc5_prg16(0, r5, true);
        mmc5_prg8(2, r6, true);
        mmc5_prg8(3, r7, false);
        break;
    default:                                    /* MMC5_PRG_8K */
        mmc5_prg8(0, r4, true);
        mmc5_prg8(1, r5, true);
        mmc5_prg8(2, r6, true);
        mmc5_prg8(3, r7, false);
        break;
    }
}

/* ------------------------------ CHR ------------------------------- */

/* $5101, in the documentation's numbering. The mode is not just how many
 * banks there are: mmc5_chr_store() and mmc5_chr_bank_for() both branch on
 * it, and they must agree, because a register written under one mode is
 * read back under the mode in force at fetch time. */
enum {
    MMC5_CHR_MODE_8K = 0,
    MMC5_CHR_MODE_4K = 1,
    MMC5_CHR_MODE_2K = 2,
    MMC5_CHR_MODE_1K = 3
};

/* The number of 1 KB CHR banks, floored at 1 because mmc5_chr_ptr() takes
 * a modulo by it (a CHR RAM cartridge reports 0 banks). */
static uint32_t mmc5_chr_banks1k(void)
{
    uint32_t banks = chr_banks * CHR_1K_PER_8K;
    return banks ? banks : 1;
}

static const uint8_t *mmc5_chr_ptr(uint32_t bank1k)
{
    return chr_base + (bank1k % mmc5_chr_banks1k()) * CHR_1K_SIZE;
}

/* Where a PPU CHR address lands, for the sprite set, the background set,
 * or whichever of the two PPUDATA sees. This is the "CHR Ax" table from
 * the MMC5 documentation written out mode by mode; the register index
 * arithmetic below is that table and nothing else uses it. */
static uint32_t mmc5_chr_bank_for(uint16_t addr, bool bg)
{
    switch (mmc5_chr_mode) {
    case MMC5_CHR_MODE_1K:                      /* 1 KB */
        /* $5128-$512B cover the background's 4 KB window at either half
         * of the PPU's address space, so only two address bits index them */
        return mmc5_chr_reg[(bg ? 8 : 0) + ((addr >> 10) & (bg ? 3 : 7))];
    case MMC5_CHR_MODE_2K: {                    /* 2 KB */
        int idx = bg ? (9 + 2 * ((addr >> 11) & 1))
                     : (1 + 2 * ((addr >> 11) & 3));
        return (uint32_t)(mmc5_chr_reg[idx] >> 1) * 2 + ((addr >> 10) & 1);
    }
    case MMC5_CHR_MODE_4K: {                    /* 4 KB */
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
 * gets 1 KB windows (nes_chr[]) and the mapper recomputes them. `set` is
 * one of the MMC5_CHR_* states, not a CHR mode: it says which of the two
 * chip windows (sprites or background) the caller is fetching for. */
static void mmc5_bind_chr(int set)
{
    for (int j = 0; j < CHR_WINDOWS; j++)
        nes_chr[j] = mmc5_chr_ptr(mmc5_chr_bank_for((uint16_t)(j * CHR_1K_SIZE),
                                                    set == MMC5_CHR_BG));
    mmc5_chr_state = (uint8_t)set;
}

/* The split screen's fixed 4 KB bank ($5202). Only the first four windows
 * are rebound: the split covers the background, and the sprite windows
 * keep whatever the sprite set had. */
static void mmc5_bind_chr4k(uint32_t bank4k)
{
    uint32_t banks = chr_banks * CHR_4K_PER_8K;
    uint32_t b = banks ? (bank4k % banks) : 0;
    for (int i = 0; i < CHR_1K_PER_4K; i++)
        nes_chr[i] = chr_base + b * CHR_4K_SIZE + (uint32_t)i * CHR_1K_SIZE;
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
    return (mmc5_ppu_ctrl & PPU_CTRL_SPRITE_16) != 0
           && (mmc5_ppu_mask & PPU_MASK_RENDER) != 0;
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

/* Writing a $512x register fills all ten internal bits, and which CPU data
 * bit goes where depends on the CHR mode in force at that moment ("the
 * 10-bit registers continue to store their value from the time they were
 * written"). The four packings, which mmc5_chr_bank_for() has to undo:
 *
 *   1 KB   $5130's two bits | the byte
 *   2 KB   $5130's one bit  | the byte << 1 | the byte's bit 0
 *   4 KB   the byte << 2    | the previous bits 2..1 | the byte's bit 0
 *   8 KB   the low 7 bits << 3 | the previous bits 2..1 | the byte's bit 0
 *
 * Those "|" are bitwise ORs, not concatenation: the bits a mode does not
 * cover keep whatever the previous value had, which is the "stored from
 * the time they were written" rule in action. It is also why a mode change
 * must not rewrite the registers: the fetch side reads the same ten bits
 * back through the new mode, so re-packing them from the old values would
 * land on different banks than the chip does. */
#define MMC5_CHR_REG_KEEP 0x06      /* the bits 4 KB and 8 KB do not cover */

static void mmc5_chr_store(int i, uint8_t v)
{
    switch (mmc5_chr_mode) {
    case MMC5_CHR_MODE_1K:
        mmc5_chr_reg[i] = (uint16_t)(((mmc5_chr_upper & MMC5_MODE_MASK) << 8)
                                     | v);
        break;
    case MMC5_CHR_MODE_2K:
        mmc5_chr_reg[i] = (uint16_t)(((mmc5_chr_upper & 1) << 9)
                                     | ((v & 0xFF) << 1) | (v & 1));
        break;
    case MMC5_CHR_MODE_4K:
        mmc5_chr_reg[i] = (uint16_t)(((v & 0xFF) << 2)
                                     | (mmc5_chr_reg[i] & MMC5_CHR_REG_KEEP)
                                     | (v & 1));
        break;
    default:                                    /* MMC5_CHR_MODE_8K */
        mmc5_chr_reg[i] = (uint16_t)(((v & 0x7F) << 3)
                                     | (mmc5_chr_reg[i] & MMC5_CHR_REG_KEEP)
                                     | (v & 1));
        break;
    }
    mmc5_chr_io_bg = (i >= 8);                  /* $5128-$512B: background */
    mmc5_chr_state = MMC5_CHR_NONE;             /* rebind on next use */
}

/* $2007 reads of the pattern tables see the set that was written last */
static uint8_t mmc5_chr_ppu_read(uint16_t addr)
{
    uint32_t bank = mmc5_chr_bank_for(addr, mmc5_chr_io_bg);
    return mmc5_chr_ptr(bank)[addr & (CHR_1K_SIZE - 1)];
}

/* --------------------------- nametables --------------------------- */

static uint8_t mmc5_bg_tile(const ppu_bg_fetch_t *f, uint8_t *tile_out,
                            uint16_t *paddr_out);

/* $5104, in the documentation's numbering. Modes %00/%01 hand ExRAM to the
 * PPU, %10/%11 take it away for the CPU. */
enum {
    MMC5_EXRAM_NT   = 0,    /* ExRAM is a fifth nametable */
    MMC5_EXRAM_ATTR = 1,    /* per-tile palette and 4 KB CHR bank */
    MMC5_EXRAM_RW   = 2,    /* the CPU owns it, read/write */
    MMC5_EXRAM_RO   = 3     /* the CPU owns it, read only  */
};

/* What $5105 can point a $2000-$2FFF page at: the two CIRAM pages, ExRAM,
 * or the synthesised fill page. */
enum { MMC5_NT_CIRAM0 = 0, MMC5_NT_CIRAM1, MMC5_NT_EXRAM, MMC5_NT_FILL };

static void mmc5_apply_nt(void)
{
    uint8_t m = mmc5_nt_map;

    for (int page = 0; page < NT_PAGES; page++) {
        switch ((m >> (2 * page)) & MMC5_MODE_MASK) {
        case MMC5_NT_CIRAM0:
            mmc5_nt_page[page] = mmc5_ciram;
            break;
        case MMC5_NT_CIRAM1:
            mmc5_nt_page[page] = mmc5_ciram + NT_PAGE_SIZE;
            break;
        case MMC5_NT_EXRAM:
            /* ExRAM, which reads as all zeros while the CPU owns it */
            mmc5_nt_page[page] = (mmc5_exram_mode <= MMC5_EXRAM_ATTR)
                                 ? mmc5_exram : mmc5_zero_page;
            break;
        default:    /* MMC5_NT_FILL: NULL selects the fill page below */
            mmc5_nt_page[page] = 0;
            break;
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
    if (mmc5_exram_mode == MMC5_EXRAM_ATTR || (mmc5_split & MMC5_SPLIT_ENABLE)
        || (m != MMC5_NT_MAP_ALL_CIRAM0 && m != MMC5_NT_MAP_ALL_CIRAM1
            && m != MMC5_NT_MAP_VERTICAL && m != MMC5_NT_MAP_HORIZONTAL)) {
        ppu_mirroring = PPU_MIRROR_MAPPER;
        ppu_bg_hook = mmc5_bg_tile;
    } else {
        ppu_bg_hook = 0;                        /* the PPU's own walk wins */
        switch (m) {
        case MMC5_NT_MAP_ALL_CIRAM0:
            ppu_mirroring = MIRROR_ONE_SCREEN_LOWER; break;
        case MMC5_NT_MAP_ALL_CIRAM1:
            ppu_mirroring = MIRROR_ONE_SCREEN_UPPER; break;
        case MMC5_NT_MAP_VERTICAL:
            ppu_mirroring = MIRROR_VERTICAL;         break;
        default:                                    /* MMC5_NT_MAP_HORIZONTAL */
            ppu_mirroring = MIRROR_HORIZONTAL;       break;
        }
    }
}

/* One nametable byte from whichever source $5105 pointed the page at. The
 * fill page has no memory behind it: $5106 answers for tiles and $5107's
 * two bits, spread over all four palette fields, for the attributes. */
static uint8_t mmc5_nt_byte(uint16_t addr)
{
    uint16_t off = (uint16_t)(addr & (NT_PAGE_SIZE - 1));
    const uint8_t *p = mmc5_nt_page[(addr >> 10) & 3];

    if (p) return p[off];
    return (off >= NT_ATTR_OFFSET) ? mmc5_fill_attr : mmc5_fill_tile;
}

/* The PPU's own path, one nametable byte at a time: used by $2006/$2007
 * and by the renderer's tile fetch. */
static uint8_t mmc5_nt_read(uint16_t addr) { return mmc5_nt_byte(addr); }

static void mmc5_nt_write(uint16_t addr, uint8_t v)
{
    uint16_t off = (uint16_t)(addr & (NT_PAGE_SIZE - 1));
    uint8_t kind = (uint8_t)((mmc5_nt_map >> (2 * ((addr >> 10) & 3)))
                             & MMC5_MODE_MASK);

    if (kind == MMC5_NT_CIRAM0 || kind == MMC5_NT_CIRAM1) {
        mmc5_ciram[(kind ? NT_PAGE_SIZE : 0) + off] = v;
    } else if (kind == MMC5_NT_EXRAM && mmc5_exram_mode <= MMC5_EXRAM_ATTR) {
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
    bool subs = (mmc5_ppu_mask & PPU_MASK_RENDER) != 0;

    mmc5_dbg_bg_hook++;

    /* ---- vertical split region: ExRAM is the nametable there ---- */
    if (subs && (mmc5_split & MMC5_SPLIT_ENABLE)
        && mmc5_exram_mode <= MMC5_EXRAM_ATTR) {
        uint8_t thr = (uint8_t)(mmc5_split & MMC5_SPLIT_COL_MASK);
        bool left = (mmc5_split & MMC5_SPLIT_RIGHT) == 0;
        int col = f->col & (NT_COLUMNS - 1);
        bool in_split = left ? (f->col < thr) : (f->col >= thr);

        if (in_split) {
            /* The split region has its own scanline counter, reset to
             * $5201 every vblank, and its own fine Y (CHR A0-A2, which is
             * what "CL mode" boards wire up). Coarse Y is five bits here,
             * so 30/31 land back on rows 0/1 of the 30-row nametable. */
            uint16_t sc = (uint16_t)(mmc5_split_scroll + f->y);
            int srow = (sc >> 3) & NT_COARSE_MASK;
            int sfy = sc & 7;
            if (srow >= NT_ROWS) srow -= NT_ROWS;
            uint16_t eoff = (uint16_t)(srow * NT_COLUMNS + col);
            uint8_t attr = mmc5_exram[NT_ATTR_OFFSET
                                      | ((srow >> 2) << 3) | (col >> 2)];

            mmc5_dbg_bg_split++;
            mmc5_use_4k_chr(mmc5_split_bank);
            *tile_out = mmc5_exram[eoff & (NT_PAGE_SIZE - 1)];
            *paddr_out = (uint16_t)(((*tile_out << 4) | sfy)
                                    & PATTERN_ADDR_MASK);
            /* palette quadrant: the row's second bit and the column's */
            return (uint8_t)((attr >> (((srow & 2) << 1) | (col & 2)))
                             & MMC5_PALETTE_MASK);
        }
        mmc5_use_bg_chr();
    }

    /* ---- extended attributes: ExRAM gives palette + 4 KB CHR bank ---- */
    if (subs && mmc5_exram_mode == MMC5_EXRAM_ATTR) {
        uint8_t e = mmc5_exram[f->nt_addr & (NT_PAGE_SIZE - 1)];
        mmc5_dbg_bg_extattr++;
        /* ExRAM's low six bits are the tile's 4 KB CHR bank and $5130's two
         * are the top of it, so $5130 has to be shifted past them. */
        mmc5_use_4k_chr(((uint32_t)(mmc5_chr_upper & MMC5_MODE_MASK) << 6)
                        | (e & MMC5_EXTATTR_BANK_MASK));
        *tile_out = tile;
        /* A4-A11 are the tile index, A10/A11 stay with the PPU; A12 and
         * up come from ExRAM + $5130, which is the 4 KB bank the mapper
         * just bound. */
        *paddr_out = (uint16_t)(((tile << 4) | f->fy) & PATTERN_ADDR_MASK);
        return (uint8_t)(e >> 6);               /* palette: the top two bits */
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
        uint16_t attr_addr = (uint16_t)(NT_BASE
                                        | (f->nt_addr & NT_PAGE_MASK)
                                        | NT_ATTR_OFFSET
                                        | ((f->nt_addr >> 4)
                                           & NT_ATTR_ROW_MASK)
                                        | ((f->nt_addr >> 2)
                                           & NT_ATTR_COL_MASK));
        uint8_t attr = mmc5_nt_byte(attr_addr);
        /* which quadrant of the block: the row's second bit and the
         * column's second bit select one of the four palette fields */
        palette = (uint8_t)((attr >> (((f->nt_addr >> 4) & 4)
                                      | (f->nt_addr & 2)))
                            & MMC5_PALETTE_MASK);
    }
    mmc5_use_bg_chr();
    *tile_out = tile;
    *paddr_out = (uint16_t)(f->pat_base + tile * TILE_BYTES + f->fy);
    return palette;
}

/* ------------------------- ExRAM, split, IRQ ---------------------- */

/* The CPU can only reach ExRAM while the PPU is drawing in modes %00/%01
 * (the chip's own rule — a game that wants to fill it during vblank must
 * switch to %10 first), read/write in %10, and read only in %11. */
static bool mmc5_exram_readable(void)
{
    return mmc5_exram_mode >= MMC5_EXRAM_RW;
}

static bool mmc5_exram_writable(void)
{
    if (mmc5_exram_mode == MMC5_EXRAM_RW) return true;
    if (mmc5_exram_mode == MMC5_EXRAM_RO) return false;
    return ppu_visible();       /* modes %00/%01: only while drawing */
}

/* $5102/$5103: the game unlocks the work RAM by writing $02 and then $01
 * ("two then one"). Until it does, writes to a RAM-mapped PRG window are
 * dropped; reads are never gated, which is why only writes call this. */
static bool mmc5_ram_writable(void)
{
    return (mmc5_prot1 & MMC5_PROT_MASK) == 2
           && (mmc5_prot2 & MMC5_PROT_MASK) == 1;
}

/* ------------------------------ writes ---------------------------- */

/* Every write is logged into a ring before anything looks at it: the host
 * tool and a human read mmc5_log to see what a cartridge actually drives,
 * and a register the emulator ignores is exactly what they are looking
 * for. The ring wraps, so the oldest entries are gone by design. */
static void mmc5_write(uint16_t addr, uint8_t v)
{
    mmc5_log[mmc5_log_n++ & (MMC5_LOG_SIZE - 1)] = ((uint32_t)addr << 8) | v;
    switch (addr) {
    case 0x5000: case 0x5001: case 0x5002: case 0x5003:
    case 0x5004: case 0x5005: case 0x5006: case 0x5007:
    case 0x5010: case 0x5011: case 0x5015:
        mmc5_dbg_wr_audio++;
        return;                                  /* audio: no APU here */

    case 0x5100: mmc5_dbg_wr_prg++;
                 mmc5_prg_mode = (uint8_t)(v & MMC5_MODE_MASK);
                 mmc5_apply_prg(); return;
    case 0x5101: mmc5_dbg_wr_chr++;
                 mmc5_chr_mode = (uint8_t)(v & MMC5_MODE_MASK);
                 /* the registers keep their ten bits; the new mode only
                  * changes how they are read back, so the windows have to
                  * be rebuilt */
                 mmc5_chr_state = MMC5_CHR_NONE; return;
    case 0x5102: mmc5_dbg_wr_prot++;
                 mmc5_prot1 = (uint8_t)(v & MMC5_PROT_MASK); return;
    case 0x5103: mmc5_dbg_wr_prot++;
                 mmc5_prot2 = (uint8_t)(v & MMC5_PROT_MASK); return;
    case 0x5104: mmc5_dbg_wr_nt++;
                 mmc5_exram_mode = (uint8_t)(v & MMC5_MODE_MASK);
                 mmc5_apply_nt(); return;
    case 0x5105: mmc5_dbg_wr_nt++;
                 mmc5_nt_map = v; mmc5_apply_nt(); return;
    case 0x5106: mmc5_fill_tile = v; return;
    case 0x5107: mmc5_fill_attr = (uint8_t)((v & MMC5_FILL_ATTR_MASK)
                                            * MMC5_FILL_ATTR_SPREAD); return;

    case 0x5113: mmc5_dbg_wr_prg++;
                 mmc5_ram_bank = (uint8_t)(v & MMC5_RAM_BANK_MASK); return;
    /* $5115 is the register the games drive, so its values are counted
     * before falling through to the shared write below. The histogram is
     * bucketed by 16 KB bank number (>> 1, the low bit of a 16 KB register
     * being ignored), which is the number a human wants to read. */
    case 0x5115: mmc5_bank_hist[(v >> 1) & (MMC5_BANK_HIST_SIZE - 1)]++;
                 /* fall through */
    case 0x5114: case 0x5116: case 0x5117:
        mmc5_dbg_wr_prg++;
        mmc5_prg_reg[addr - 0x5114] = v;
        mmc5_apply_prg();
        return;

    case 0x5130: mmc5_dbg_wr_chr++;
                 mmc5_chr_upper = (uint8_t)(v & MMC5_MODE_MASK);
                 mmc5_chr_state = MMC5_CHR_NONE;    /* changes the bank the
                                                     * extended attributes
                                                     * point at */
                 return;

    case 0x5200: mmc5_dbg_wr_split++;
                 mmc5_split = v; mmc5_apply_nt(); return;
    case 0x5201: mmc5_dbg_wr_split++; mmc5_split_scroll = v; return;
    case 0x5202: mmc5_dbg_wr_split++; mmc5_split_bank = v; return;
    case 0x5203: mmc5_dbg_wr_irq++; mmc5_irq_target = v; return;
    case 0x5204: mmc5_dbg_wr_irq++;
                 mmc5_irq_enable = (v & MMC5_STATUS_IRQ) != 0; return;
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
            mmc5_exram[addr & (MMC5_EXRAM_SIZE - 1)] = v;
            mmc5_dbg_wr_exram++;
        } else {
            /* modes %00/%01 outside the picture: the chip drops it */
            mmc5_dbg_exram_blank_drop++;
        }
        return;
    }
    /* A $5114-$5116 register can map work RAM into a PRG window; then a
     * write there is a RAM write and the ROM registers never see it. The
     * offset comes from the same wrap mmc5_prg_ram() used, so read and
     * write land in the same byte. */
    if (addr >= 0x8000) {
        int win = (addr >> 13) & 3;
        if (mmc5_win_ram[win] && mmc5_ram_writable())
            mmc5_ram[mmc5_win_ram_off[win] + (addr & (PRG_8K_SIZE - 1))] = v;
    }
}

static uint8_t mmc5_read(uint16_t addr)
{
    if (addr == 0x5204) {
        /* the "in frame" bit is PPU /RD activity, not a latch: it is set
         * while visible lines are being drawn and clear otherwise */
        uint8_t r = (uint8_t)((mmc5_irq_pending ? MMC5_STATUS_IRQ : 0)
                              | (ppu_visible() ? MMC5_STATUS_IN_FRAME : 0));
        mmc5_dbg_rd_5204++;
        mmc5_irq_pending = false;                /* reading acknowledges */
        return r;
    }
    if (addr == 0x5205) return (uint8_t)(mmc5_mult & 0xFF);
    if (addr == 0x5206) return (uint8_t)(mmc5_mult >> 8);
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        mmc5_dbg_rd_exram++;
        return mmc5_exram_readable() ? mmc5_exram[addr & (MMC5_EXRAM_SIZE - 1)]
                                     : 0;
    }
    return 0;                                    /* open bus */
}

/* Power-on defaults, from the chip's documentation. Two of them matter to
 * real cartridges: $5117 comes up as $FF (the last bank, where the reset
 * vector is) and PRG mode 3, which is what Koei's games rely on before
 * they write a single register. The protection registers come up locked. */
static void mmc5_reset(void)
{
    mmc5_prg_mode = MMC5_PRG_8K;
    mmc5_chr_mode = MMC5_CHR_MODE_1K;
    mmc5_prot1 = 1;                 /* writes are locked out until the
                                     * game writes $02/$01 */
    mmc5_prot2 = 2;
    mmc5_exram_mode = MMC5_EXRAM_RO;
    mmc5_nt_map = 0;
    mmc5_fill_tile = 0;
    mmc5_fill_attr = 0;
    mmc5_ram_bank = 0;
    for (int i = 0; i < MMC5_PRG_REGS; i++) mmc5_prg_reg[i] = 0;
    mmc5_prg_reg[3] = 0xFF;         /* $5117 powers up at $FF: last bank */
    for (int i = 0; i < MMC5_CHR_REGS; i++) mmc5_chr_reg[i] = 0;
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
    for (int i = 0; i < PRG_WINDOWS; i++) mmc5_win_ram[i] = false;
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
    /* [11] is a bit field rather than a register, because the four flags
     * are what a human asks about: 1 IRQ enabled, 2 IRQ pending, 4 the PPU
     * is drawing, 8 the last $512x write was the background's set. */
    mmc5_regs_dbg[11] = (uint8_t)((mmc5_irq_enable ? 1 : 0)
                                  | (mmc5_irq_pending ? 2 : 0)
                                  | (ppu_visible() ? 4 : 0)
                                  | (mmc5_chr_io_bg ? 8 : 0));
    mmc5_regs_dbg[12] = mmc5_ppu_ctrl;
    mmc5_regs_dbg[13] = mmc5_ppu_mask;
    mmc5_regs_dbg[14] = (uint8_t)mmc5_mult;
    mmc5_regs_dbg[15] = (uint8_t)(mmc5_mult >> 8);
    for (int i = 0; i < MMC5_CHR_REGS; i++) mmc5_chr_dbg[i] = mmc5_chr_reg[i];
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
        mmc5_dbg_publish();         /* every line, because tools/swd.py
                                     * reads the dump asynchronously */
        mmc5_scanline(y);
        return;
    }
    if (mapper_num != MAPPER_MMC3)
        return;

    /* Reload when the counter has run out or when $C001 asked for it, and
     * fire on zero *after* that: with a latch of N the IRQ lands N lines
     * after the reload, while a latch of 0 fires on the reload line
     * itself. Games tune their split against exactly this. */
    if (mmc3_irq_counter == 0 || mmc3_irq_reload) {
        mmc3_irq_counter = mmc3_irq_latch;
        mmc3_irq_reload = false;
    } else {
        mmc3_irq_counter--;
    }

    if (mmc3_irq_counter == 0 && mmc3_irq_enabled)
        mmc3_irq_flag = true;
}

/* Each chip acknowledges in its own way: the MMC3 by writing $E000 and the
 * MMC5 by reading $5204, so the flag cannot be cleared here. */
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
    prg_banks = prg_size / PRG_16K_SIZE;
    if (prg_banks == 0) prg_banks = 1;  /* a truncated file still has to
                                         * produce a bank pointer */
    chr_base = chr;
    chr_banks = chr_size / CHR_8K_SIZE; /* 0 for a CHR RAM cartridge */

    shift_reg = shift_count = 0;
    mmc1_control = MMC1_CTRL_PRG_MODE_MASK;     /* PRG mode 3: last bank
                                                 * fixed, which is where the
                                                 * reset vector is */
    mmc1_chr0 = mmc1_chr1 = mmc1_prg = 0;

    for (int i = 0; i < 8; i++) mmc3_regs[i] = 0;
    mmc3_select = 0;
    mmc3_irq_latch = mmc3_irq_counter = 0;
    mmc3_irq_reload = mmc3_irq_enabled = mmc3_irq_flag = false;

    uxrom_bank = 0;

    /* Each chip brings up a different part of itself: MMC5 from its own
     * power-on defaults, MMC3 from the register file (mode 0, R6/R7 at 0),
     * the rest from a fixed bank. */
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

/* Only the MMC5 puts anything in $4020-$5FFF; for every other cartridge
 * that window is open bus, which the machine layer renders as 0. */
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
    if (addr == 0x2001) mmc5_ppu_mask = v;
    mmc5_chr_state = MMC5_CHR_NONE;     /* the binding depends on both */
}

/* ------------------------------- writes --------------------------- */

/* MMC3's four register regions, each a pair split by the address's low
 * bit. $8000 and $8001 both re-run mmc3_apply(), because the select
 * register's mode bits change what the bank numbers mean. */
static void mmc3_write(uint16_t addr, uint8_t value)
{
    bool odd = (addr & 1) != 0;

    if (addr < 0xA000) {                        /* $8000-$9FFF */
        if (!odd) {
            mmc3_wr_8000++;
            mmc3_select = value;
            mmc3_apply();
        } else {
            mmc3_wr_8001++;
            mmc3_regs[mmc3_select & MMC3_SELECT_REG_MASK] = value;
            mmc3_apply();
        }
    } else if (addr < 0xC000) {                 /* $A000-$BFFF */
        if (!odd) {
            mmc3_wr_A000++;
            mmc3_mirror_reg = value;
            mmc3_apply_mirror(value);
        }
        /* odd ($A001): PRG RAM protection. The 8 KB here is the machine's
         * work RAM, which stays readable and writable; on these boards the
         * protection only silences writes, and this emulator does not model
         * the silencing. */
    } else if (addr < 0xE000) {                 /* $C000-$DFFF */
        if (!odd) {
            mmc3_wr_C000++;
            mmc3_irq_latch = value;
        } else {
            mmc3_wr_C001++;
            mmc3_irq_counter = 0;
            mmc3_irq_reload = true;             /* reload on the next line */
        }
    } else {                                    /* $E000-$FFFF */
        if (!odd) {
            mmc3_wr_E000++;
            mmc3_irq_enabled = false;
            mmc3_irq_flag = false;              /* acknowledge */
        } else {
            mmc3_wr_E001++;
            mmc3_irq_enabled = true;
        }
    }

    mmc3_regs_dbg[0] = mmc3_regs[0];             /* readable over SWD */
    mmc3_regs_dbg[1] = mmc3_regs[1];
    mmc3_irq_latch_dbg = mmc3_irq_latch;
}

static void uxrom_write(uint8_t value)
{
    /* Bus conflicts, which a real board has (the value is ANDed with the
     * ROM byte it overwrites), are not modelled: games avoid them by
     * construction. See the file header. */
    uxrom_bank = (uint8_t)(value & UXROM_BANK_MASK);
    uxrom_apply();
}

/* The MMC1's serial port. Each write shifts in bit 0 (LSB first), the
 * fifth completes a register whose identity comes from the address, and a
 * write with bit 7 set resets the sequence wherever it had got to — the
 * reason a game can abandon a half-written byte. Bit 7 also forces the
 * PRG mode bits, which is why the reset path is an OR and not a plain
 * assignment to the control register. */
static void mmc1_write(uint16_t addr, uint8_t value)
{
    if (value & MMC1_SERIAL_RESET) {
        shift_reg = 0;
        shift_count = 0;
        mmc1_control |= MMC1_CTRL_PRG_MODE_MASK;
        mmc1_apply(mmc1_control);
        return;
    }

    shift_reg |= (uint8_t)((value & 1) << shift_count);
    if (++shift_count < MMC1_SERIAL_BITS)
        return;

    switch ((addr >> 13) & 3) {                 /* A13-A14 pick the register */
    case MMC1_REG_CONTROL: mmc1_control = shift_reg; break;
    case MMC1_REG_CHR0:    mmc1_chr0    = shift_reg; break;
    case MMC1_REG_CHR1:    mmc1_chr1    = shift_reg; break;
    default:               mmc1_prg     = shift_reg; break;
    }
    shift_reg = 0;
    shift_count = 0;
    mmc1_apply(mmc1_control);
}

/* The dispatch order is the memory map's: MMC5 first because it answers in
 * $5000-$5FFF as well as $8000-$FFFF; then the ROM-area registers, which
 * NROM does not have. */
void mapper_write(uint16_t addr, uint8_t value)
{
    if (mapper_num == MAPPER_MMC5) {
        mmc5_write(addr, value);        /* $5000-$5FFF and $8000-$FFFF */
        return;
    }

    if (addr < 0x8000)
        return;                 /* no other mapper has registers down here */

    if (mapper_num == MAPPER_MMC3) {
        mmc3_write(addr, value);
        return;
    }

    if (mapper_num == MAPPER_UXROM) {
        uxrom_write(value);
        return;
    }

    if (mapper_num != MAPPER_MMC1)
        return;                 /* NROM ignores writes to the ROM area */

    mmc1_write(addr, value);
}

