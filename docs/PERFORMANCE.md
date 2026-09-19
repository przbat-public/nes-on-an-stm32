# Performance — where a frame goes

Everything below was measured on the board with the DWT cycle counter
(80 MHz), read over SWD while the emulator runs. **Prince of Persia (UxROM)
is the reference cartridge**: its attract mode changes scene every few
hundred frames, so two builds are compared at the same *emulated frame
numbers* (`python3 tools/swd.py track`), never just "after boot".

The arc, light scene / heavy scene on that cartridge:

| what landed | light scene | heavy scene |
|---|---|---|
| the first proper per-frame budget | 46.5 ms (21 fps) | 55 ms (18 fps) |
| the background renderer rewritten | 28.4 ms (35 fps) | 37.7 ms (26 fps) |
| bands double-buffered, PPU rendering into the framebuffer | 25.2 ms (39.7 fps) | 33.1 ms (30 fps) |
| the exact band comparison (current build) | 20.3–21.0 ms (47–48 fps) | 33 ms (30 fps) |

The display link is the wall: 122,880 bytes a frame at 40 MHz SPI is
**24.6 ms** of wire time (18.4 ms in the optional 12-bit mode), so this link
cannot go much past ~45 fps (54 at 12 bits) whatever the emulation costs.
What each step cost and bought follows in the order it happened.

## The first budget (self-test cartridge)

Numbers are host cycles per emulated NES frame (29,780 emulated CPU cycles),
from the era before the counters were published at the frame boundary — the
totals and the frame rates held up, the split did not (see *The frame,
closed*):

| Phase | Before optimisation | After | Share now |
|---|---|---|---|
| Emulating the 6502 | 1,815,000 (22.7 ms) | 1,595,000 (19.9 ms) | 37% |
| Rendering the PPU (240 lines) | 2,573,000 (32.2 ms) | 1,679,000 (21.0 ms) | 39% |
| Converting bands to RGB565 | ~660,000 (8.3 ms) | ~650,000 (8.1 ms) | 15% |
| Frame overhead, DMA waits | ~410,000 (5.1 ms) | ~430,000 (5.4 ms) | 9% |
| **Total** | **5,462,000 (68 ms) → 14.6 fps** | **4,351,000 (54 ms) → 18.4 fps** | |

The SPI wire time (122,880 bytes at 40 MHz = 24.6 ms) is **not** in the
table because it is overlapped: each band is transmitted while the next one
is being emulated. It also cannot be overlapped with itself, which is the
ceiling discussed under *Why not 60 fps*.

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

## What was left, and what happened to it

- **A fast `$2002` path** (the CPU's 37% in that table) was never taken: the
  polling read still walks `bus_read → nes_bus_read_slow → ppu_read_reg`.
  What moved the core instead was the register work described under *The
  macro version* below.
- **The PPU loop** was rewritten exactly as sketched here: eight pixels at a
  time out of prebuilt tables, with the two pattern bitplanes packed into
  one 2-bit-per-pixel word first.
  → background 19.6 ms → 8.2 ms a frame.
- **The band conversion** now stores 16-bit values four pixels at a time,
  through a palette table indexed by the raw framebuffer byte.
  → 3.94 ms → 3.37 ms.
- **"A second staging buffer is impossible"** was true of the *DMA* (one
  channel feeds `SPI1_TX`), not of the CPU: the firmware now alternates two
  staging buffers and converts the next band while the previous one is still
  on the wire, with 4-line bands so the pair costs the same RAM as the one
  8-line buffer it replaced.
- **The 25–30 fps target** was passed: 47–48 fps in the light scenes, 30 in
  the heavy ones, against the ~45 fps the display link allows (see the top
  of this document).

## Why not 60 fps

Two hard limits:

1. **The display link.** 61,440 pixels × 2 bytes at 40 MHz SPI is 24.6 ms of
   wire time per frame, and it cannot be overlapped with itself. A
   full-screen update therefore tops out at ~40 fps in theory — ~54 fps in
   the 12-bit mode — and skipping unchanged bands is what brings the
   measured ceiling to roughly 45 fps. The STM32L4 cannot clock SPI1 faster
   than f_PCLK/2 = 40 MHz, so this is the end of that road.
2. **The 80 MHz CPU** has ~1.33 million cycles per 60 Hz frame. Emulating a
   6502 plus a scanline PPU in that budget is possible only with heavy
   optimisation (the classic approach is even/odd frame rendering or a
   smaller internal resolution).

30 fps — the speed many PAL NES owners remember anyway — is now the *heavy*
scene's rate, and the light scenes are past it. 60 fps is not a target this
display link can meet.

## Measuring it yourself

Every counter in `.bss` is published once per frame, at the frame boundary,
so one SWD read is one complete frame — no differencing, no "since boot"
numbers. `tools/swd.py` reads them through openocd's telnet port:

```bash
python3 tools/swd.py read          # one sample
python3 tools/swd.py budget 5      # per-frame breakdown over a 5 s window
python3 tools/swd.py track 300 600 # the counters at given emulated frames
python3 tools/swd.py stats 10 1    # mean/min/max over ten samples
python3 tools/swd.py watch 30      # a sample every 2 s
python3 tools/swd.py list          # symbol -> address, from emu.map
```

`dbg_fps` (the rate the firmware measures from its own cycle counter) is also
printed in the top-left corner of the picture for the first few seconds after
boot. To flash a new build while openocd is holding the ST-Link, see
*Flashing while openocd is running* below.

## Measured again, on the board (Prince of Persia, UxROM)

This was the first per-frame measurement on a real game, and one of its
numbers was wrong. The `dbg_cyc_cpu` and `dbg_cyc_flush` rows held up; the
PPU row did not: 83,881 cycles (1.0 ms) had been carried over from an older
build and an older, mostly static scene. Frame-aligned on this cartridge the
renderer was really costing 1.7–2.0 million cycles — the largest item in the
frame. The "16 ms that appears in no counter" below is the same mistake seen
from the other side, and *The frame, closed* is how it was tracked down.

| stage | cycles | time |
|---|---|---|
| 6502 core (`dbg_cyc_cpu`) | 1 447 077 | 18.1 ms |
| PPU scanline renderer (`dbg_cyc_ppu`) | 83 881 | 1.0 ms ← the wrong figure, see above |
| band conversion + staging (`dbg_cyc_flush`) | 1 192 137 | 14.9 ms |

One thing in that picture was right, and it is a hard ceiling: **the display
link is a floor.** A frame is 61,440 pixels = 122,880 bytes over SPI at
40 MHz = **24.6 ms of wire time**, no matter how fast the emulation gets.
The other thing it suggested — that ~15 ms a frame was being spent *after*
`nes_run_frame()` waiting for the last band — was an artefact of the same bad
premise: the DMA wait turned out to be 0.02 ms, because the transfer hides
behind the emulation (see *The frame, closed*).

What was already cut by then: the band conversion swaps bytes in the palette
once at boot (`pal_sw[]`) and writes 16-bit values four pixels at a time,
instead of two 8-bit stores per pixel with a shift each. Prince of Persia
went from 20 to 22 fps, and `lcd_conv_selfcheck()` verifies the byte order at
every boot against bytes worked out from the NES palette by hand
(`dbg_lcd_conv_ok`), because a wrong byte order here would show up only as
wrong colours on the panel.

## Bands that did not change are not sent

From the second frame on, a band is converted and sent only if it differs
from what the panel holds: no conversion, no SPI traffic. That matters twice
— the conversion costs CPU time *and* the transfer costs wire time, and the
wire is the ceiling described above.

The panel cannot be asked what it holds (story 18 in
[docs/BRINGUP.md](BRINGUP.md)), so the firmware snapshots each band into
`held[]` just before the renderer overwrites those framebuffer rows — at that
instant the framebuffer *is* what the panel holds — and `push_band()`
compares the 1 KB band against it, exactly, word-wise. Snapshot plus
comparison is 1.67 ms a frame (0.52 + 1.16) against the 1.97 ms of the two
passes it replaced, and it *saves* several milliseconds more on a static
screen, because bands the old heuristic would have sent are not sent at all.

**The first version of that test was wrong** and produced horizontal stripes
on the panel; the story is in [docs/BRINGUP.md](BRINGUP.md) (17), and the
before/after with the proof is in *The stripes* at the end of this document.

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

The two sections that follow are what happened when that was tried: first
with the locals as file-scope *statics* (worth nothing on the board), then as
true automatic variables (126 → 102 host cycles per instruction). Story 15
in [docs/BRINGUP.md](BRINGUP.md) tells the same thing as a war story.

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

## The frame, closed (and why it looked like 16 ms was missing)

The counters were being read in a way that mixed two different things:

* `dbg_cyc_cpu`, `dbg_cyc_ppu` and `dbg_cyc_frame` are *snapshots* of the
  last frame;
* `dbg_cyc_flush` (and `dbg_cyc_band`/`dbg_cyc_wait`/`dbg_bands_sent`/
  `dbg_bands_skipped`) accumulated **since boot**, so a single read of them
  says nothing about a frame.

Comparing the snapshots against the totals is what produced the "16 ms
that appears in no counter" — and the ~84,000-cycle PPU figure in the
table further up is from an older build/scene; on the board the renderer
was really costing 1.7–2.0 million cycles, i.e. the *largest* item in the
frame, not the smallest.

Every counter in `.bss` is now published once per frame, at the frame
boundary (`lcd_dbg_frame()`, `ppu_dbg_frame()`, the end of
`nes_run_frame()`), so one SWD read is one complete frame and the budget
closes without any differencing. The two pieces of the loop that had no
counter at all — the display hook and the per-line bookkeeping around it —
now have one (`dbg_cyc_hook`, `dbg_cyc_loop`), and the hook is split into
copy / change check / DMA wait / conversion / window setup. Reading them is:

    python3 tools/swd.py budget 5      # per-frame table from a 5 s window
    python3 tools/swd.py track 400 600 # the same numbers at given frames
    python3 tools/swd.py read          # one sample

`track` exists because Prince of Persia's attract mode changes scene every
few hundred frames and the frame cost changes with it (a light scene is
46 ms, a heavy one 56 ms). Comparing two builds at the same *emulated
frame number* is the only fair way to do it.

### The closed budget — before

Measured over 5 s on the board, Prince of Persia, `dbg_cyc_frame` per frame,
80 MHz:

| counter | cycles/frame | ms | share |
|---|---|---|---|
| `dbg_cyc_cpu` | 1 111 862 | 13.90 | 24.6% |
| `dbg_cyc_ppu` | 2 078 858 | 25.99 | 46.0% |
| `dbg_cyc_flush` | 1 253 731 | 15.67 | 27.7% |
| unaccounted (loop, IRQ, bookkeeping) | 75 085 | 0.94 | 1.7% |
| **`dbg_cyc_frame`** | **4 519 536** | **56.49** | **17.7 fps** |

Same build, frame-aligned at emulated frames 400–1000 (a lighter scene):
`dbg_cyc_frame` 3 719 746 (46.5 ms) = cpu 792 109 (9.9 ms) + ppu 1 659 294
(20.7 ms) + everything in the display hook 1 268 343 (15.9 ms). 21 fps.

So the missing 16 ms was the display hook plus a PPU that costs 20 ms, not
1 ms. The DMA wait (`dbg_cyc_wait`) was 0.02 ms: **the transfer was already
hidden**; the wire time (24.6 ms of traffic, ~21 ms of it actually sent
because unchanged bands are skipped) is overlapped with the emulation.

### What was changed

1. **The background renderer** (`ppu.c`), the largest item. It expanded one
   pixel at a time — 19 instructions and two data-dependent branches per
   pixel. Now the two pattern bytes are packed into one 16-bit
   two-bits-per-pixel word through a 256-entry table (`sprd[]`, 512 bytes)
   and expanded to eight palette bytes four nibbles at a time through
   `pair[8][16]` (256 bytes, rebuilt whenever palette RAM is written), with
   four 16-bit stores. Zero pixels need no branch: `ppu_line` is pre-filled
   with the backdrop and writing that value back is what leaving the pixel
   alone used to do.
   → background 19.6 ms → 8.2 ms, pixel-for-pixel identical.

   One trap came out of this and is worth writing down. Pixel value 0 of
   *every* background palette shows the universal backdrop at $3F00, not
   that palette's own entry 0 — the colour a cartridge writes to $3F04/
   $3F08/$3F0C is never displayed. The first version of the table used
   `pal_cache[palette*4]`, and **the frame-for-frame comparisons did not
   catch it**, because all three test cartridges (and Prince of Persia at
   the point they were compared) write the same colour to all four backdrop
   entries. `tools/ppu_expand_test.c` (`make host-ppu-test`) now compares
   the table against the old per-pixel loop exhaustively — every (lo, hi)
   pair, all four palettes, with a palette RAM where nothing coincides:
   262,144 checks, 0 failed with the fix, 176,925 failed without it.
2. **The backdrop fill** in the same function: the byte loop compiled to a
   `memset` call (271 cycles a line); it is an unrolled word fill now.
   → 0.81 ms → 0.59 ms.
3. **The scanline copy and the "did this band change" test** (`lcd.c`) were
   two byte-wise passes over the same 61,440 bytes. The change test is now
   accumulated, word at a time, *while* the scanline is copied into the
   framebuffer, so it costs nothing extra. The firmware proves the two
   agree at every boot: for the first 8 frames it redoes the byte-wise
   comparison and checks it says "unchanged" exactly when the accumulated
   word does, and that a band it just sent really did land in the shadow
   (`dbg_lcd_band_ok`, 240 bands checked, reads 1).
   → copy + change check 10.6 ms → 2.5 ms.
4. **The band conversion** looks its palette up through `pal_sw_idx[256]`
   (a 512-byte table indexed by a raw framebuffer byte) instead of masking
   every pixel first. → 3.94 ms → 3.37 ms. `lcd_conv_selfcheck()` now
   verifies *that* table, since it is the one the panel sees.
5. `cycles_now()` is a `static inline` in `hal.h`: the frame accounting
   calls it a few thousand times a frame and a call to `hal.c` cost more
   than the counter it reads.

Not worth doing, and measured rather than guessed: **a second staging
buffer** (the DMA wait is 0.02–2.6 ms a frame and the wait that remains is
the wire time itself), and **a taller band** (`set_window`, three commands
plus two data phases 30 times a frame, costs 0.25 ms — even BAND_H 16
would save 0.12 ms for 4 KB of RAM).

### The closed budget — after

Same board, same cartridge, 5 s window, and a frame-aligned column at the
same emulated frames:

| counter | cycles/frame | ms | share |
|---|---|---|---|
| `dbg_cyc_cpu` | 767 736 | 9.60 | 33.8% |
| `dbg_cyc_ppu` | 727 572 | 9.09 | 32.1% |
| — `dbg_cyc_fill` (backdrop) | 50 397 | 0.63 | |
| — `dbg_cyc_bg` (tiles) | 657 054 | 8.21 | |
| — `dbg_cyc_spr` (sprites) | 6 480 | 0.08 | |
| `dbg_cyc_hook` (display hook) | 709 675 | 8.87 | 31.3% |
| — `dbg_cyc_copy` | 163 748 | 2.05 | |
| — `dbg_cyc_diff` (change check) | 36 192 | 0.45 | |
| — `dbg_cyc_wait` (DMA) | 198 974 | 2.49 | |
| — `dbg_cyc_conv` | 269 550 | 3.37 | |
| — `dbg_cyc_setwin` | 19 925 | 0.25 | |
| `dbg_cyc_loop` (mapper, IRQ, NMI) | 53 189 | 0.66 | 2.3% |
| **`dbg_cyc_frame`** | **2 268 414** | **28.36** | **35.4 fps** |

`UNACCOUNTED` is 0.13 ms (0.5%) — the frame is closed. Three 4 s samples
gave 35.85 / 35.59 / 35.38 fps for this scene.

Before and after at the *same emulated frames* (the only fair comparison,
and three valid samples per build):

| emulated frame | before `cyc_frame` | after `cyc_frame` | before fps | after fps |
|---|---|---|---|---|
| 400 | 3 786 478 (47.3 ms) | 2 268 460 (28.4 ms) | 21 | 35 |
| 600 | 3 696 366 (46.2 ms) | 2 289 771 (28.6 ms) | 21 | 34 |
| 800 | 3 699 190 (46.2 ms) | 2 290 976 (28.6 ms) | 21 | 34 |
| 1000 | 3 696 949 (46.2 ms) | 2 288 838 (28.6 ms) | 21 | 34 |
| 1200 | 4 399 862 (55.0 ms) | 3 018 334 (37.7 ms) | 18 | 26 |
| 1400 | 4 463 676 (55.8 ms) | 3 007 771 (37.6 ms) | 17 | 26 |

(The last two rows sit in a scene where the game itself does much more CPU
work — 1.35 M cycles against 0.77 M — and the frame rate of the *demo* also
shifts which scene the attract mode is showing, so the last two rows are
the loosest part of the comparison; the first four are stable to under 1%
across runs of both builds.)

`dbg_cyc_ppu` over the same frames: 1 741 815 → 721 074 (light scene) and
2 028 115 → 1 009 271 (heavy scene). Wall-clock samples right after boot: 35.85 / 35.59 / 35.38 fps after
(21 fps before, same scene, frame-aligned); in the game's heavier scenes
26-30 fps after against 17-18 before.

### What is left, and the ceiling

`dbg_cyc_cpu` is now the largest single item — 9.6 ms in a light scene,
13.5-14.2 ms in a heavy one — at ~82-84 host cycles per emulated
instruction in this window. Past that the wall is the display link again: 25
bands × 4 KB at 40 MHz is 20.5 ms of wire time a frame, so even with free
emulation this build cannot go much past ~45 fps. `dbg_cyc_wait` (2.5 ms)
is already the emulation running out of work rather than the transfer
being late.

### The last full window

A 5 s `tools/swd.py budget` window on the board (Prince of Persia, 16-bit,
80 MHz). These are the counters of the build just before the exact band
comparison — the `copy` and `difference` rows are the "before" column of
*The stripes* below, which replaced them with 0.52 + 1.16 ms. The sub-parts
are as the firmware reports them and do not sum to the hook exactly; the
remainder is the band loop's own bookkeeping.

| part | ms/frame |
|---|---|
| `dbg_cyc_cpu` | 9.85 |
| `dbg_cyc_ppu` | 8.04 |
| — background tiles (`dbg_cyc_bg`) | 7.61 |
| — backdrop fill (`dbg_cyc_fill`) | 0.07 |
| — sprites (`dbg_cyc_spr`) | 0.08 |
| `dbg_cyc_hook` (the display) | 6.48 |
| — band snapshot (`dbg_cyc_copy`) | 1.47 |
| — the skip decision (`dbg_cyc_diff`) | 0.50 |
| — DMA wait (`dbg_cyc_wait`) | 0.15 |
| — conversion (`dbg_cyc_conv`) | 3.61 |
| — window setup (`dbg_cyc_setwin`) | 0.49 |

`dbg_cyc_wait` at 0.15 ms against 3.61 ms of conversion is the point of the
double buffering: the transfer is hidden behind the emulation, and the
conversion is the CPU work that is left. `dbg_cyc_cpu` is still the largest
single item. Frame-aligned on the current build the light scene is 20.3–21.0
ms (47–48 fps); this window mixes attract-mode scenes, which is why its
counters sit above the light-scene figures.

### Measured and dropped

Worth recording as method — each of these was measured on the board with the
counters above, and each was left out:

| change | result |
|---|---|
| `-O3` instead of `-O2` | ~3% **slower** |
| `-Os` instead of `-O2` | ~20% slower |
| unrolling the change check to 16 pixels | noise, no gain |
| removing the backdrop fill on its own | a wash — until the two edge tiles were expanded through `pair[]` as well, which turned it into 0.63 ms → 0.07 ms |

### The guard rails that were run

| check | result |
|---|---|
| `make host-test` | 19 checks, 0 failed |
| `make` (arm-none-eabi-gcc, `-Wall -Wextra`) | 0 warnings, 0 errors |
| `make host-ppu-test` (new) | 262,144 checks, 0 failed |
| `make host-rom ROM=build/test.nes FRAMES=60` | 0 differing bytes (checksum 0586B1E4) |
| `make host-rom ROM=build/mmc3.nes FRAMES=90` | 0 differing bytes (checksum 8097F62C) |
| `make host-rom ROM=build/mmc1.nes FRAMES=60` | 0 differing bytes (checksum BCD8C7DC) |
| MMC3 self-test, `$030F` (host and on the board) | 3F, PASS |
| `dbg_lcd_conv_ok` after boot | 1 |
| `dbg_lcd_band_ok` after boot | 1 (240 bands: that build still had 8-line bands; today's check covers 480) |
| `tools/diff_run.sh 5000000 HEAD` | 5,000,105 instructions, identical state hashes |

(The 6502 core itself was not touched; the differential core harness was
run anyway and reports the working tree's core identical to the committed
one. The capture of the three reference frames was taken before any
change and lives in `build/ref/`.)

### Flashing while openocd is running

`st-flash` cannot open the ST-Link while `openocd` holds it — it fails with
"another process has device opened for exclusive access" and, with the
output piped, quietly does nothing. Use the openocd telnet port instead:

    tools/ocd_flash.sh emu.bin        # reset halt, write, verify, reset run

## The stripes: a skip decision that compared against the wrong band

The symptom, on the panel: horizontal stripes in the lower, largely static
part of a Prince of Persia dungeon, looking like a copy of what was above
them. The framebuffer was correct, so it was in the display path.

### What the code did

`push_band()` decided with `band_xor`, an OR of `(new scanline ^ shadow)`
accumulated by the per-line copies, where the shadow `sent` was **one band**
(1 KB) and was overwritten by every push. The PPU now renders straight
into the framebuffer, so there is no scanline copy to compare against, and
what the accumulation actually measured was "is this band the same as the
band **above** it" (the last one pushed, row-aligned) — not "is this band
the same as what the panel holds at this position". On a static screen,
where consecutive bands are identical (exactly the lower half of a dungeon
screen), that answers "unchanged" for a band the panel does not have: the
band is skipped, the panel keeps older content, and the stripe is
permanent. The boot check compared against the same wrong shadow, so it
agreed and never caught it.

### Why the obvious fix does not fit

Keeping a real shadow of the panel and comparing the 1 KB band against it
word-wise needs 61,440 more bytes. The part has 96 KB of SRAM1 and 32 KB
of SRAM2; the framebuffer is 60 KB of SRAM1 and the mappers' 8 KB work RAM
plus 8 KB CHR RAM are another 16 KB, so a second full frame would need
146 KB of 128 KB. It does not link, at any `-O` level.

### The fix: one band of shadow, taken at the right moment

The shadow is not needed, because at the moment a band is **started** the
framebuffer still holds exactly what the panel holds — the previous frame
ended with the two in step (every band that changed was pushed) and the
fps overlay writes both together. So `lcd_nes_line_target()` copies that
one band (1 KB, four contiguous rows) into `held[]` just before the
renderer overwrites it, and `push_band()` sends the band if and only if it
differs from `held[]`:

    panel == framebuffer            at the start of a band
    band changed  <=>  fb_band_now != held

`band_xor` is gone entirely — there is no second mechanism to be
consistent with. The comparison is `band_differs()`, word-wise, 32 bytes
per iteration, exact, and any difference at all means "send".

### What it costs

Board, Prince of Persia, 16-bit, same emulated frames as the build before
(`tools/swd.py track`), 80 MHz:

| | before (ecf705e) | after |
|---|---|---|
| `dbg_cyc_copy` (snapshot) | 117,921 (1.47 ms) | 41,280 (0.52 ms) |
| `dbg_cyc_diff` (the decision) | 39,827 (0.50 ms) | 92,520 (1.16 ms) |
| both passes | 157,748 (1.97 ms) | **133,800 (1.67 ms)** |
| `dbg_cyc_frame` at frame 300 | 1,998,247 (24.98 ms, 41 fps) | 1,626,113 (20.33 ms, 47 fps) |
| `dbg_cyc_frame` at frame 600 | 2,016,527 (25.21 ms, 39 fps) | 1,676,244 (20.95 ms, 48 fps) |
| `dbg_cyc_frame` at frame 900 | 2,020,071 (25.25 ms, 39 fps) | 1,647,228 (20.59 ms, 48 fps) |

The exact comparison is **1.16 ms a frame** — 120 KB read across the 60
bands, in a frame where all of them are scanned in full, which is the
static case that matters — and 0.52 ms for the snapshot, so the whole
change-detection pass is 1.67 ms against the 1.97 ms of the heuristic it
replaces. The frame is ~4.4 ms **faster** than before for a second reason:
the old rule sent 49 of 60 bands a frame in scenes where nothing changed
(its neighbour comparison says "different" whenever the picture varies
vertically), and the exact rule sends none of them. In the 5 s budget
sample with 51 bands a frame actually changing, `dbg_cyc_diff` reads
1.60 ms and the frame is 33.3 ms.

### The proof, part 1: the panel cannot be read back on this shield

The ST7789 has a memory-read command (0x2E, RAMRD) that clocks frame
memory back out of SDO, and the shield is supposed to wire SDO to
PA6/MISO. The firmware implements it (`lcd_readback_check()`: set window,
0x2E, one dummy byte, then compare the converted framebuffer band against
the bytes the panel returns; counters `dbg_lcd_readback_*`). It does not
work here, and the measurement is in the counters:

* `dbg_lcd_readback_probe` = **0** — a four-pixel pattern written into the
  left black bar and clocked back at 40, 10, 2.5 and 0.31 MHz, at both
  possible byte alignments, never matches. What comes back is a 3-byte
  periodic pattern that changes between two reads of *identical* content.
* `dbg_lcd_probe_miso` = 0, and `hal_miso_probe()` samples PA6 as a GPIO
  input: with the STM32's internal **pull-up** the line reads high in
  26–28 of 32 samples, with the internal **pull-down** it reads low (6 of
  32). The line follows the pull, so nothing is driving it — the shield
  does not connect the panel's SDO to MISO.

So the readback verdict here is always "differs" (`dbg_lcd_readback_diff`
≈ 90,000 of 122,880 bytes of line noise, `dbg_lcd_readback_ok` = 0), and
that is what it should say. The path stays compiled in — it is the right
check on a board where SDO *is* wired — and its cost is why it only runs
once after boot (`LCD_READBACK_FRAME`) or when `dbg_lcd_readback_req` is
written over SWD (a whole frame is ~50 ms of SPI traffic in each
direction).

### The proof, part 2: an invariant and a stress test, on the hardware

Since the panel cannot be asked, the firmware checks its own decisions.
Two instruments, both in `lcd.c`:

* **The referee** (`dbg_lcd_invariant_*`) keeps a 32-bit fingerprint per
  band of the content last handed to the panel, and requires **every
  band that is skipped** to still match it. A wrong reference — the bug
  above — makes a changed band look unchanged and lands here with
  probability 1 − 2⁻³² per band. It costs ~1.5 ms a frame, so it runs
  through the boot window, whenever `dbg_lcd_stress` is running, and
  whenever `dbg_lcd_check_req` is left set over SWD.
* **The stress test** (`dbg_lcd_stress`, a frame budget written over SWD)
  paints a known 48×4 block into one band of the lower half of the
  picture *and pushes it to the panel*, so the panel really holds the
  artificial content, and the next frame the emulator draws its own
  picture over that band — which must then be sent back.
  `dbg_lcd_stress_missed` counts injected bands that were not (a band that
  changed and was skipped); injections that happen to paint the colour
  already there are not counted, since they change nothing.

Same detector, two builds, Prince of Persia, on the board:

| check | old decision (band vs band above) | this build |
|---|---|---|
| `dbg_lcd_band_ok` after boot | 0 (the byte-wise check disagreed) | **1** (480 bands) |
| `dbg_lcd_invariant_ok` / `_bad` | 0 / 55,524 of 57,943 | **1 / 0 of 135,897** |
| `dbg_lcd_stress_missed` of injections | 1,033 of 1,200 | **0 of 900** (and 0 of 2,500 in a longer run) |

That is the bug reproduced and the fix confirmed without a human looking at
the panel: with the old decision 86–96% of the bands that provably changed
were not sent, which is what the stripes were.

### The guard rails that were run for this change

| check | result |
|---|---|
| `make host-test` | 19 checks, 0 failed |
| `make host-ppu-test` | 262,144 checks, 0 failed |
| `make` and `make LCD_12BIT=1` (arm-none-eabi-gcc, `-Wall -Wextra`) | 0 warnings, 0 errors |
| `make host-rom ROM=build/test.nes FRAMES=60` | 0 differing bytes (sha256 eef92aeb…) |
| `make host-rom ROM=build/mmc1.nes FRAMES=60` | 0 differing bytes (sha256 18c1ae2f…) |
| `make host-rom ROM=build/mmc3.nes FRAMES=90` | 0 differing bytes (sha256 fc71b2f6…) |
| MMC3 self-test `$030F`, host and board | 3F / 3F, PASS |
| board: `dbg_lcd_conv_ok`, `dbg_lcd_band_ok`, `dbg_lcd_invariant_ok` | 1, 1, 1 (`_bad` = 0) |
| board: `dbg_lcd_readback_ok` | 0 — SDO is not wired, see above |

(The three reference frames were captured from a pristine `git archive
HEAD` checkout before any change, and the three cartridges are regenerated
byte-identically by `tools/make_test_rom.py`; `build/ref/` is not committed,
and the references were kept in `/tmp` for that session.)

## The 12-bit panel mode

`make LCD_12BIT=1` builds the display path for the ST7789's RGB444 mode
(COLMOD 0x53): two pixels in three bytes, six nibbles in the order
p0.R, p0.G, p0.B, p1.R, p1.G, p1.B, each channel quantised by *rounding to
nearest*. That is the right inverse of the way the panel expands a nibble
back to six bits, and it halves the worst-case channel error against
truncation (8/255 against 13/255). The format is chosen at compile time; the
default is the 16-bit path, and for now it stays the default.

It is **not a win yet**. Measured on the board, the 12-bit build is 1.0–1.2%
*slower* than the 16-bit one on the same cartridge: the frame's CPU work
(~25 ms) is still above the wire time of the 12-bit frame it was measured in
(15.4 ms; a full 12-bit frame would be 18.4 ms), so shaving the wire does not
shorten the frame — and the packing costs more conversion than it saves (one
32-bit store per two pixels, but two table lookups and a 3-byte stride
instead of two 16-bit stores).

The ceiling is worth stating anyway, because it is a property of the link and
not of this build: 122,880 bytes a frame at 40 MHz is 24.6 ms of wire (18.4 ms
at 12 bits), so this display tops out near **45 fps (54 at 12 bits)** whatever
the emulation costs. The 12-bit mode becomes interesting when the emulation
fits inside its wire time — roughly, when the CPU work drops below 15 ms —
and it costs 4 bits per channel instead of 5/6/5.

The mode carries its own boot self-check (`lcd_conv_selfcheck()`, reported in
`dbg_lcd_conv_ok` and `dbg_lcd_conv_checks`): six hand-worked bytes for four
pixels — so the middle byte, which holds two different pixels' nibbles, is
exercised — plus all 64 palette entries against an independently written
rounding expression. Both ARM builds (`make` and `make LCD_12BIT=1`) are
warning-free.
