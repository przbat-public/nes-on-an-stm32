/*
 * nesmem.h — the memory the CPU and PPU hit most often, plus inline
 * accessors for it.
 *
 * Why a header: every 6502 memory access and every pattern-table fetch
 * goes through these. While they were plain function calls, the profile
 * showed the emulated CPU burning ~210 host cycles per instruction.
 * Inlining the two hot regions (2 KB of work RAM and the cartridge ROM)
 * removes almost all of that cost.
 */
#pragma once
#include <stdint.h>

/* console RAM, mirrored over $0000-$1FFF */
extern uint8_t nes_ram_2k[0x800];

/* PRG-ROM: mapped at $8000, masked for NROM-128 mirroring */
extern const uint8_t *nes_prg;
extern uint32_t       nes_prg_mask;

/* CHR: either ROM or the cartridge's RAM */
extern const uint8_t *nes_chr;
extern uint8_t       *nes_chr_ram;
extern int            nes_chr_is_ram;

/* fast paths (RAM + PRG); everything else goes to the slow handlers */
uint8_t nes_bus_read_slow(uint16_t addr);
void    nes_bus_write_slow(uint16_t addr, uint8_t value);

static inline uint8_t bus_read(uint16_t addr)
{
    if (addr < 0x2000)
        return nes_ram_2k[addr & 0x7FF];
    if (addr >= 0x8000)
        return nes_prg[(addr - 0x8000) & nes_prg_mask];
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

/* pattern table / CHR fetch, used by the PPU thousands of times per frame */
static inline uint8_t chr_read(uint16_t addr)
{
    addr &= 0x1FFF;
    return nes_chr_is_ram ? nes_chr_ram[addr] : nes_chr[addr];
}
