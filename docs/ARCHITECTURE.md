# Architecture — how the emulator is put together

The project is a small stack of layers. Each one has a single job, and
only the bottom layer knows about hardware registers — which is what made
it possible to develop the whole emulator on a PC first and only then
flash it.

```
                      main.c
        init, load the cartridge from flash, run frames
                         │
        ┌────────────────┼─────────────────┐
        │                │                 │
     nes.c            lcd.c             input.c
  the machine      the picture        the pad
  bus, mappers,    landscape,         joystick ->
  controller,      RGB565 / RGB444    NES buttons
  frame loop       bands over SPI DMA
        │                │
        ├───────┬────────┘
        │       │
     cpu6502.c  ppu.c
     6502 core  scanline renderer
        │       │
        └───┬───┘
          hal.c        ← the only file with registers
     clocks, GPIO, SPI, DMA, cycle counter
```

`mapper.c` sits beside `nes.c`: it owns the bank pointers the bus reads
through (NROM, MMC1, UxROM, MMC3) and the MMC3 scanline counter.

## The CPU core (`cpu6502.c`)

A straightforward interpreter: 256-entry dispatch, all official opcodes
plus the common undocumented ones (LAX/SAX/DCP/ISC/SLO/RLA/SRE/RRA).
Every instruction ends by recording how many cycles it burned, because
the frame loop paces the CPU in **cycles per scanline**, not instructions
per frame:

```c
for (y = 0; y < 262; y++) {
    cpu_run(341 * (y + 1) / 3 - 341 * y / 3);   /* 113.67 cycles/line */
    ...render the line...
    ...vblank and NMI at line 241...
}
```

The dispatch itself is **generated** by `tools/gen_6502.py` from an
opcode matrix — 256 `case` labels with the right addressing helper and
cycle count. Hand-writing that table is where CPU emulators usually get
their bugs; generating it removes the whole class.

The generated file also owns the *shape* of the loop. `cpu_ops.h` is
`#include`d **inside** `cpu_run()`, so the 6502 registers can be automatic
variables there — `pc` and `a` in ARM registers, `x`/`y`/`p` packed into one
word — with every bus accessor, addressing mode, stack op and ALU helper
reaching them as a macro. That is what took the core from 126 host cycles per
emulated instruction (14.8 ms a frame) to **102** (9.7 ms). A first attempt
that cached the same registers in file-scope statics was worth nothing on the
board: GCC may not keep a static in a register across a bus access it cannot
prove does not alias it. War story 15 in [docs/BRINGUP.md](BRINGUP.md).

Memory access goes through `bus_read()`/`bus_write()`. On the board the
two hot regions (2 KB of work RAM and the cartridge ROM) are **inlined**
(`nesmem.h`), because the profiler showed the emulated CPU burning ~210
host cycles per instruction when every access was a function call; inlining
those two regions is what brought it to 126, and the register work to 102.
`$2002` polling still walks the slow path — a measured remaining cost, kept
deliberately for now.

## The PPU (`ppu.c`)

A **scanline renderer**: for every visible line it produces 256 pixels
from the current scroll position, then the frame loop advances the PPU's
internal address register exactly like the real chip does.

Implemented:

- nametables with horizontal/vertical mirroring, attribute tables,
- background with the real fine-X/coarse-X behaviour,
- sprites 8×8 and 8×16, horizontal/vertical flips, priority, the
  8-sprites-per-line limit and the overflow flag,
- **sprite-0 hit** (games use it to split the screen),
- the loopy registers `v/t/x/w` and the pre-render `t → v` latch, which
  is what makes scrolling work at all,
- buffered `$2007` reads and palette mirroring,
- NMI on vblank.

Three details that cost real performance work:

1. **Tile addressing**: the renderer walks the nametable incrementally
   (one VRAM index per tile column) instead of recomputing the address
   per tile, and flips the 1 KB nametable bit when the column wraps.
2. **Sprites are fetched per sprite-row, not per pixel** — the pattern
   bytes for a visible sprite's scanline are read once into an 8-pixel
   row buffer, then blended. Fetching two bytes per pixel through a
   function pointer was one of the biggest costs in the frame.
3. **The background is expanded through tables, two bitplanes at a time.**
   A tile row's two pattern bytes are packed into one 16-bit
   2-bit-per-pixel word (`sprd[]`, 256 entries) and expanded into eight
   palette bytes through `pair[8][16]`, four nibbles at a time, with 16-bit
   stores. Pixel value 0 is special: it means the **universal backdrop** at
   `$3F00`, never that palette's own entry 0. The table maps it there
   directly, and `tools/ppu_expand_test.c` (`make host-ppu-test`) checks the
   tables against the old per-pixel loop over every pattern-byte pair and all
   four palettes, with palette RAM seeded so no two entries coincide
   (262,144 checks). The frame comparisons could not have caught a mistake
   here — all the test cartridges write the same colour to all four backdrop
   entries. War story 16 in [docs/BRINGUP.md](BRINGUP.md).

The renderer writes each line into the row the display layer hands it
(`nes_line_target`) — on the board, the display framebuffer itself — so the
picture is written once, not rendered into a line buffer and then copied.

## The mappers (`mapper.c`)

Cartridges are not flat memory: games switch banks while they run. The
mapper layer publishes **bank pointers** instead of touching the bus, and
the inline accessors read through them, so a bank switch costs a few
pointer writes and nothing else. The windows are deliberately small —
**four 8 KB PRG windows** (`$8000`, `$A000`, `$C000`, `$E000`) and **eight
1 KB CHR windows** — because MMC3 swaps half of what NROM and MMC1 treat
as one unit; the index is a shift and a mask in the hot path.

- **NROM** (mapper 0): pointers set once — 16 KB carts mirror the single
  bank at both `$8000` and `$C000`, 32 KB carts map straight through.
- **MMC1** (mapper 1): five-bit serial writes to `$8000-$FFFF`; the
  address bits pick the register (control / CHR bank 0 / CHR bank 1 /
  PRG bank). The control register's low bits select the nametable
  arrangement — including the two **one-screen** modes, which is why the
  PPU has four mirroring settings rather than two — while bits 2-3 pick
  one of the four PRG banking modes and bit 4 the CHR bank size.
- **UxROM** (mapper 2): one register. Its low bits select a 16 KB PRG bank
  at `$8000`; the last bank stays fixed at `$C000`, so the reset and
  interrupt vectors never move. Prince of Persia is one, and it has 8 KB of
  CHR **RAM** instead of CHR ROM — the machine layer already fetched CHR
  through pointers, so CHR RAM needed no change there.
- **MMC3** (mapper 4): a register file rather than a serial port. `$8000`
  picks a register (and swaps the PRG and CHR halves), `$8001` writes it:
  `R0`/`R1` are the 2 KB CHR banks, `R2`-`R5` the 1 KB ones, `R6`/`R7`
  the two switchable 8 KB PRG banks — the other two always point at the
  last banks of the cartridge, which is what keeps the reset and
  interrupt vectors mapped. `$A000` sets the mirroring, `$A001` protects
  the work RAM.
- **The MMC3 scanline counter** (`$C000`-`$E001`) is the part games use to
  split the screen: a down counter reloaded from a latch, clocked **once
  per rendered scanline** (it is the PPU's pattern fetches that clock it,
  so it only ticks while the picture is being drawn), raising an IRQ at
  zero. `nes.c` clocks it after every visible line and hands the
  interrupt to `cpu_irq()`, which honours the I flag and leaves the
  request pending until the game acknowledges it by writing `$E000`.

## The machine (`nes.c`)

The NES memory map with its quirks: 2 KB of RAM mirrored four times, PPU
registers mirrored every 8 bytes, `$4014` OAM DMA (which stalls the CPU
for 513 cycles — those cycles are charged to the CPU budget, otherwise
the emulated CPU runs far too fast), cartridge work RAM and the PRG-ROM
window with NROM-128 mirroring.

`nes_load()` parses an iNES image: PRG/CHR pointers, mirroring, mapper
check. Mappers 0 (NROM), 1 (MMC1), 2 (UxROM) and 4 (MMC3) are implemented —
between them a large part of the library, and everything in the
verified-cartridge table in the [README](../README.md).

## The picture path (`lcd.c` + DMA)

The panel is addressed in **landscape** (MADCTL MV): a 320×240 window
with the 256×240 NES picture centred; the side bars are painted black
once at boot. The framebuffer holds the picture as NES colour indices
(61,440 bytes), and the PPU renders straight into it through
`nes_line_target` — a frame is written once, not rendered into a line
buffer and then copied (that copy alone was ~2 ms a frame).

Two pixel formats are compiled in, chosen at build time: **RGB565**
(16-bit, the default) and **RGB444** (`make LCD_12BIT=1`, two pixels in
three bytes, 25% less wire time, 4 bits per channel). 12-bit is not faster
yet — the CPU work is still above its wire time — so the 16-bit path is the
default; [docs/PERFORMANCE.md](PERFORMANCE.md) has the numbers.

Streaming works in **4-scanline bands**, double-buffered:

```
render lines 0..3  → convert band 0 → start DMA → render lines 4..7
     → convert band 1 → wait for band 0 (normally long gone) → start DMA → ...
```

One DMA channel feeds `SPI1_TX`, so only one transfer may be in flight —
starting a second one would overwrite the first one's registers. Two staging
buffers alternate instead, so the next band is converted while the previous
one is still on the wire; 4-line bands are what makes the pair cost the same
RAM as the single 8-line buffer they replaced. A band (2 KB in 16-bit mode)
is 0.41 ms on the wire, shorter than the emulation that produces its four
lines, so the DMA is not what the CPU waits for.

**A band is sent only if it changed.** At the moment a band is *started* the
framebuffer still holds exactly what the panel holds, so
`lcd_nes_line_target()` snapshots those four rows into `held[]` just before
the renderer overwrites them, and `push_band()` sends the band if and only if
it differs from that copy, word-wise. A wrongly skipped band is a stale
stripe on the panel and invisible in the framebuffer — that bug happened — so
the decision is guarded from several independent directions at boot and on
demand over SWD (`dbg_*` in `.bss`):

| guard | question it answers |
|---|---|
| `lcd_conv_selfcheck()` → `dbg_lcd_conv_ok`, `dbg_lcd_conv_checks` | are the bytes about to go on the wire the ones the NES palette and the panel format call for? (hand-worked bytes plus all 64 entries, in whichever mode was built) |
| `dbg_lcd_band_ok`, `dbg_lcd_band_checked` | does the fast word-wise decision agree with the obvious byte-wise one? (first 8 frames after boot) |
| `dbg_lcd_invariant_ok` / `_bad` | the **referee**: every band that is *skipped* still matches a 32-bit fingerprint of what the panel was last given |
| `dbg_lcd_stress*` | the **stress test**: inject a block into a band, push it to the panel, and require the emulator to send that band back |
| `dbg_lcd_readback_ok` / `dbg_lcd_readback_diff` | ask the panel itself (RAMRD) — always 0 on this shield, because its SDO is not wired to MISO |

The referee and the stress test are how the band-skip fix was proven with no
eyes on the panel; stories 17 and 18 in [docs/BRINGUP.md](BRINGUP.md).

## The pad (`input.c`)

The joystick is soldered to the shield, so its four contacts are fixed to
the board — but the picture is drawn in landscape, which means the board
is held a quarter turn counter-clockwise from the portrait hold used in
the mini-mario project. The stick turns with the board, so each direction
lands on a different pin than its name suggests:

| pushed by the player | pin | was, in the portrait hold |
|---|---|---|
| up | PB0 | right |
| right | PB4 | down |
| down | PB6 | left |
| left | PC0 | up |

The table in `input.c` is exactly the portrait map rotated 90°, so one
turn of the stick in the hand is one turn of the D-pad in the game.

Five switches have to cover the NES's eight inputs, so the blue button
(PC13) carries different bits depending on the combination, and on the
**layout** chosen in `input_init()` — holding the button while the board
comes out of reset picks the second one:

| layout | A | B | START |
|---|---|---|---|
| gamepad (default) | blue | blue + down | blue + up, held |
| shooter | blue + up | blue | blue + down, held |

The shooter layout exists because games like Contra put fire on B: it is
held constantly while running, so it has to sit on the bare button. START
is always the combination held for **12 frames** — about a third of a second
at the speeds this build reaches, a little longer in the heavy scenes —
which keeps a tap of fire while ducking from pausing the game. The NES's own
protocol (`$4016` strobe + shift) is emulated in `nes.c`, so the cartridge
sees a normal controller.

A real gamepad is the obvious next step, and the honest gap in this project:
five switches and two layouts are a workaround, not a controller. The plan is
a wired USB pad with a Raspberry Pi Pico acting as USB host and emulating the
4021 shift register the NES pad protocol needs, or an SNES pad wired straight
to the board — no code for either exists yet (see the
[README](../README.md)).

## Testing strategy

Everything here is reproducible from the repository — nothing needs a
cartridge, and only the last two rows need the board:

| Level | Command | What it proves |
|---|---|---|
| CPU unit test | `make host-test` | 19 checks on the 6502 core, run on the PC with a flat 64 KB bus |
| Core equivalence | `tools/diff_run.sh 5000000 HEAD` | random instruction streams (batches, reset, NMI, IRQ, DMA stalls) through this tree's core and one extracted from a git revision produce identical state hashes; the full campaign was 35 million instructions over seven seeds, plus an exhaustive sweep of all 256 opcodes |
| PPU expander | `make host-ppu-test` | the background tables against the old per-pixel loop, exhaustively: every pattern-byte pair × every palette, 262,144 checks |
| Self-test cartridges | `make rom`; `python3 tools/make_test_rom.py --mmc1` / `--mmc3` | NROM, MMC1 and MMC3 features, each verdict left in RAM at `$0300`-`$030F`; `tools/mmc3_result.c` checks the MMC3 byte on the host |
| Emulator on the PC | `make host-rom ROM=... FRAMES=...` | renders frames to a PNG with the same `ppu.c`/`nes.c` as the board, prints a frame checksum and the RAM results |
| Hardware | `make flash` + a framebuffer dump | the board's picture matches the PC reference frame |
| On the board, live | `python3 tools/swd.py read\|budget\|track\|stats\|watch` | the emulated CPU's state, the per-frame counters and the boot self-checks, read over SWD while the game runs |

The hardware comparison is the important one: the same cartridge is run on
both sides and the framebuffer is compared byte for byte — 0 differing bytes
of 61,440 is the pass mark for every cartridge in the README's table. A
one-frame misalignment already shows up as ~4% of pixels differing, so the
test is sharp. The joystick and the pad bits can also be driven and read from
the debugger, which is how the control layout was verified without a hand on
the stick.

[docs/BUILD.md](BUILD.md) has the commands in full;
[docs/PERFORMANCE.md](PERFORMANCE.md) explains what the counters mean.
