# Performance — where the 54 ms of a frame go

Measured on the board with the DWT cycle counter (80 MHz), running the
self-test cartridge. Numbers are host cycles per emulated NES frame
(29,780 emulated CPU cycles):

| Phase | Before optimisation | After | Share now |
|---|---|---|---|
| Emulating the 6502 | 1,815,000 (22.7 ms) | 1,595,000 (19.9 ms) | 37% |
| Rendering the PPU (240 lines) | 2,573,000 (32.2 ms) | 1,679,000 (21.0 ms) | 39% |
| Converting bands to RGB565 | ~660,000 (8.3 ms) | ~650,000 (8.1 ms) | 15% |
| Frame overhead, DMA waits | ~410,000 (5.1 ms) | ~430,000 (5.4 ms) | 9% |
| **Total** | **5,462,000 (68 ms) → 14.6 fps** | **4,351,000 (54 ms) → 18.4 fps** | |

The SPI wire time (61,440 bytes at 40 MHz = 12.3 ms) is **not** in the
table because it is overlapped: each band is transmitted while the next
one is being emulated.

## What was optimised

1. **Inlined memory access** (`nesmem.h`). Every 6502 memory access and
   every CHR fetch used to be a function call, sometimes through a
   function pointer. Work RAM and the cartridge ROM now have inline fast
   paths; only the PPU/APU/IO window goes through the slow handler.
   → CPU emulation 12% faster.

2. **Palette cache.** The renderer looked up every one of the 61,440
   pixels' colours through `ppu_read_vram()` (a call with mirroring
   maths). The 32 palette bytes now live in a masked cache that the
   renderer indexes directly.
   → part of the PPU gain.

3. **Incremental nametable walk.** Instead of computing a VRAM address
   per tile (with the mirroring function), the renderer walks the
   nametable with one index increment per column and flips the 1 KB
   nametable bit when the column wraps.

4. **Sprites fetched per row, not per pixel.** A visible sprite's
   scanline is read once into an 8-pixel row buffer (with the flips
   applied), then blended over the background. Previously each pixel did
   two pattern fetches through a function pointer.
   → PPU rendering 35% faster in total.

## What is left

- **CPU (37%)**: the test cartridge spends most of its time polling
  `$2002` (waiting for vblank), which currently walks
  `bus_read → nes_bus_read_slow → ppu_read_reg → switch`. A fast inline
  path for `$2002` would cut a few hundred thousand cycles per frame.
  A jump-table-free opcode dispatch (computed goto / 256 direct cases)
  is another option.
- **PPU (39%)**: the background loop is already close to one store per
  pixel; the remaining cost is bit extraction. Rendering 8 pixels at a
  time from a 256-entry lookup table per pattern byte pair would help.
- **Band conversion (15%)**: 61,440 palette lookups with two byte stores
  each. Storing 16 bits at a time (as one `uint16_t` write) would halve
  the stores.
- **A second staging buffer is impossible** (one DMA channel), but a
  larger band (16 lines) would reduce the per-band overhead.

Realistic target with the above: **25–30 fps**.

## Why not 60 fps

Two hard limits:

1. The display link: 61,440 bytes at 40 MHz SPI = 12.3 ms per frame, and
   it cannot be overlapped with itself. That caps a full-screen update at
   ~80 fps in theory, but any real rendering on top brings it down.
2. The 80 MHz CPU has ~1.33 million cycles per 60 Hz frame. Emulating a
   6502 plus a scanline PPU in that budget is possible only with heavy
   optimisation (the classic approach is even/odd frame rendering or a
   smaller internal resolution).

30 fps — the speed many PAL NES owners remember anyway — is the sensible
target, and it is within reach of the optimisations listed above.

## Measuring it yourself

The firmware keeps four counters readable over SWD while it runs:

```bash
# SWD addresses come from the map file
grep -E "dbg_cyc_(cpu|ppu|flush|frame)" emu.map
```

plus `dbg_frames` (frames rendered) and `dbg_fps` (the rate the firmware
itself measures from the cycle counter), which is also printed in the
top-left corner of the picture for the first few seconds after boot.
