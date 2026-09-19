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

## Measured again, on the board (Prince of Persia, UxROM)

Counters are the DWT cycle counter at 80 MHz, read over SWD while the game
runs; `dbg_cyc_*` in `.bss` carries them.

| stage | cycles | time |
|---|---|---|
| 6502 core (`dbg_cyc_cpu`) | 1 447 077 | 18.1 ms |
| PPU scanline renderer (`dbg_cyc_ppu`) | 83 881 | 1.0 ms |
| band conversion + staging (`dbg_cyc_flush`) | 1 192 137 | 14.9 ms |

Two things follow from that, and one of them is a hard ceiling:

1. **The display link is a floor.** A frame is 61,440 pixels = 122,880 bytes
   over SPI at 40 MHz = **24.6 ms of wire time**, no matter how fast the
   emulation gets. 30 fps means a 33 ms budget, so emulation plus
   conversion has to fit in ~8 ms — that is a different class of work than
   the current 33 ms.
2. **The rest of the frame is outside `nes_run_frame()`**: `dbg_cyc_frame`
   is measured inside the frame loop, and a 20 fps game spends roughly
   15 ms per frame after it returns — mostly `lcd_nes_frame_end()` waiting
   for the last band to leave the wire, plus the per-frame bookkeeping.
   That wait is what the band skipping below attacks.

What was already cut: the band conversion now swaps bytes in the palette
once at boot (`pal_sw[]`) and writes 16-bit values four pixels at a time,
instead of two 8-bit stores per pixel with a shift each. Prince of Persia
went from 20 to 22 fps, and `lcd_conv_selfcheck()` verifies the byte order
at every boot against bytes worked out from the NES palette by hand
(`dbg_lcd_conv_ok`), because a wrong byte order here would show up only as
wrong colours on the panel.

## Bands that did not change are not sent

`push_band()` now keeps a copy of what the panel was last given and skips
a band whose 2 KB are identical: no conversion, no SPI traffic. On Prince
of Persia about a fifth to a quarter of the bands are skipped
(`dbg_bands_sent` / `dbg_bands_skipped` in `.bss`, read over SWD), which
matters twice — the conversion costs CPU time *and* the transfer costs
wire time, and the wire is the ceiling the section above describes.

The frame rate on that cartridge stays in the 19-22 fps range either way,
so the honest headline is that this change helps static screens and does
not rescue a scrolling one: what is left is the 6502 core (~15 ms) and the
conversion of the bands that do change (~8-10 ms). Those are the next two
targets, and the self-test cartridges plus the frame comparisons in the
README are the guard rails for both.

## What the 6502 core actually costs per instruction

Measured on the board with the game running (Prince of Persia), by reading
`cpu.instructions`, `cpu.cycles` and `dbg_frames` over SWD:

| quantity | value |
|---|---|
| instructions per frame | 9 389 |
| 6502 cycles per frame (`cpu.cycles`) | 30 214 |
| host cycles per emulated instruction | **126** |

The frame accounting is sound (30 214 cycles is what a 1.79 MHz 6502 does
in a 60th of a second, and 3.2 cycles per instruction is right), so the
core is faithful — it is just slow: 126 cycles of a Cortex-M4 per
instruction is two to four times what a switch-based interpreter should
need. At 45 cycles per instruction the same work would cost about 5 ms a
frame instead of 15, which is the difference between 20 and 28 fps on this
cartridge.

The reason is visible in the code: every opcode reaches the 6502 registers
through the global `cpu` struct (`cpu.a`, `cpu.pc`, ...), and because the
bus accesses in between touch memory, the compiler cannot keep them in
registers across an instruction — each field becomes a load and a store,
several times per opcode. The fix is the standard one: copy the registers
into locals at the top of `cpu_run()`, run the whole batch of instructions
against those locals, and write them back at the end (and in `cpu_nmi()` /
`cpu_irq()`, which change them from outside). That is a mechanical change
across `cpu6502.c` and the generated `cpu_ops.h`, guarded by the 19 CPU
tests and the pixel-for-pixel frame comparisons the project already has.

**Ceiling, for the record:** 122,880 bytes per frame at 40 MHz SPI is
24.6 ms of wire time. Even with a free CPU that is ~40 fps; 30 fps (33 ms)
needs the emulation plus the band conversion to fit inside the wire time.

## The register cache, and why it did not help on the board

The core was restructured so a whole batch of instructions runs against
cached registers (`reg_pc/reg_a/...`, `exec_one()` inlined into
`cpu_run()`, `cpu` synced at the batch boundaries only). On the host that
is worth ~11% of the core and ~5% of a frame, and it is verified
equivalent: 19/19 CPU checks, 0 differing pixels on the NROM and MMC3
cartridges, and a differential harness that ran 11.6 million random
instructions through both cores with identical state hashes.

On the board it is worth nothing. Measured over SWD with the game running:
**131 host cycles per emulated instruction, against 126 before** — the
same, i.e. noise.

The reason is in the storage class: those cached registers are file-scope
*statics*, not true locals. A C compiler may not keep a static object in a
register across a memory access it cannot prove does not alias it, and the
core's bus reads and writes go to arbitrary addresses, so GCC spills and
reloads all six around every single one. To actually win on ARM the
registers have to be automatic variables *inside* `cpu_run()`, with the
bus helpers and the generated opcode bodies reaching them as macros rather
than as functions with static operands — which is the same shape every
fast 6502 interpreter uses. That is the next attempt, and the differential
harness from this one is the tool to verify it with.

## The macro version: true locals, and it did help

The registers are now automatic variables inside `cpu_run()` — `pc` in an
ARM register, `a` in another, `x`/`y`/`p` packed into a single word — and
every bus accessor, addressing mode, stack op and ALU helper reaches them
as a macro defined inside that function. The generated `cpu_ops.h` emits
the whole dispatch loop for that reason (a `#include` cannot live inside a
macro body, as both compilers pointed out).

Measured over SWD on the board, same game, same method as above:

| | before | statics attempt | **macros** |
|---|---|---|---|
| host cycles per emulated instruction | 126 | 131 | **102** |
| core cost per frame | 14.8 ms | 14.8 ms | **9.7 ms** |

So ~19% off the core, ~5 ms off a frame. The frame rate on this cartridge
barely moves (21 fps) because the core is no longer the biggest item: the
band conversion and the display link are, and the link's 24.6 ms of wire
time per frame is the ceiling that no CPU work can go below.

What makes this version work where the previous one did not is visible in
the disassembly: `cpu` is touched once in the prologue and once in the
epilogue, and the loop body keeps the state in `r3`-`r5`/`r8` with no
loads or stores to the struct — the only spill on a straight-line path is
the accumulator saved across a call to the slow bus path.

Two things came out of this that are worth keeping: `tools/diff_test.c`
with `tools/diff_setup.py` and `tools/diff_run.sh`, a differential harness
that runs random instruction streams (mixing `cpu_step`, batches, reset,
NMI, IRQ and DMA stalls) through the working tree's core and one extracted
from a git revision and compares state hashes — 35 million instructions
over seven seeds, plus an exhaustive sweep of all 256 opcodes; and
`tools/mmc3_result.c`, which checks the MMC3 self-test's result byte.
