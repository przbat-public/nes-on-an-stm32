/*
 * nesmem.h — the memory the CPU and PPU hit most often, plus inline
 * accessors for it.
 *
 * Why a header: every 6502 memory access and every pattern-table fetch
 * goes through these. While they were plain function calls, the profile
 * showed the emulated CPU burning ~210 host cycles per instruction.
 * Inlining the two hot regions (2 KB of work RAM and the cartridge ROM)
 * removed almost all of that cost.
 *
 * The cartridge is addressed through *bank pointers* rather than a base
 * address, because MMC1 carts switch 16 KB PRG and 4 KB CHR banks at
 * runtime; the mapper keeps these four pointers up to date.
 */
#pragma once
#include <stdint.h>

/* console RAM, mirrored over $0000-$1FFF */
extern uint8_t nes_ram_2k[0x800];

/* PRG: four 8 KB windows, at $8000/$A000/$C000/$E000. Eight kilobytes and
 * not sixteen because MMC3 swaps half of such a window at a time. */
extern const uint8_t *nes_prg[4];

/* CHR: eight 1 KB windows over $0000-$1FFF (MMC3's smallest CHR bank) */
extern const uint8_t *nes_chr[8];
extern uint8_t       *nes_chr_ram;
extern int            nes_chr_is_ram;

/* slow paths (PPU, APU, IO); RAM and ROM are inlined below */
uint8_t nes_bus_read_slow(uint16_t addr);
void    nes_bus_write_slow(uint16_t addr, uint8_t value);

static inline uint8_t bus_read(uint16_t addr)
{
    if (addr < 0x2000)
        return nes_ram_2k[addr & 0x7FF];
    if (addr >= 0x8000)
        return nes_prg[(addr >> 13) & 3][addr & 0x1FFF];
    return nes_bus_read_slow(addr);
}

static inline void bus_write(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) {
        nes_ram_2k[addr & 0x7FF] = value;
        return;
    }
    nes_bus_write_slow(addr, value);
}

/* pattern table fetch, used by the PPU thousands of times per frame */
static inline uint8_t chr_read(uint16_t addr)
{
    addr &= 0x1FFF;
    if (nes_chr_is_ram)
        return nes_chr_ram[addr];
    return nes_chr[addr >> 10][addr & 0x3FF];
}
