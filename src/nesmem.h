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

/* PRG: two 16 KB windows ($8000 and $C000) */
extern const uint8_t *nes_prg_lo;
extern const uint8_t *nes_prg_hi;

/* CHR: two 4 KB windows ($0000 and $1000), or 8 KB of CHR RAM */
extern const uint8_t *nes_chr_lo;
extern const uint8_t *nes_chr_hi;
extern uint8_t       *nes_chr_ram;
extern int            nes_chr_is_ram;

/* slow paths (PPU, APU, IO); RAM and ROM are inlined below */
uint8_t nes_bus_read_slow(uint16_t addr);
void    nes_bus_write_slow(uint16_t addr, uint8_t value);

static inline uint8_t bus_read(uint16_t addr)
{
    if (addr < 0x2000)
        return nes_ram_2k[addr & 0x7FF];
    if (addr >= 0x8000) {
        const uint8_t *bank = (addr < 0xC000) ? nes_prg_lo : nes_prg_hi;
        return bank[addr & 0x3FFF];
    }
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
    {
        const uint8_t *bank = (addr < 0x1000) ? nes_chr_lo : nes_chr_hi;
        return bank[addr & 0x0FFF];
    }
}
