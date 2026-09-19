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
  bus, NROM,       landscape,        joystick ->
  controller,      RGB565 bands      NES buttons
  frame loop       over SPI DMA
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

Memory access goes through `bus_read()`/`bus_write()`. On the board the
two hot regions (2 KB of work RAM and the cartridge ROM) are **inlined**
(`nesmem.h`), because the profiler showed the emulated CPU burning ~210
host cycles per instruction when every access was a function call.

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

Two details that cost real performance work:

1. **Tile addressing**: the renderer walks the nametable incrementally
   (one VRAM index per tile column) instead of recomputing the address
   per tile, and flips the 1 KB nametable bit when the column wraps.
2. **Sprites are fetched per sprite-row, not per pixel** — the pattern
   bytes for a visible sprite's scanline are read once into an 8-pixel
   row buffer, then blended. Fetching two bytes per pixel through a
   function pointer was one of the biggest costs in the frame.

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
check. Mappers 0 (NROM), 1 (MMC1) and 4 (MMC3) are implemented — between
them a large part of the library.

## The picture path (`lcd.c` + DMA)

The panel is addressed in **landscape** (MADCTL MV): a 320×240 window
with the 256×240 NES picture centred; the side bars are painted black
once at boot. The framebuffer holds the picture as NES colour indices,
so the PPU writes indices straight in.

Streaming works in **8-scanline bands**:

```
render lines 0..7  → convert band 0 to RGB565 → start DMA → render 8..15
     → wait for band 0 (already done) → convert → start DMA → ...
```

One DMA channel feeds `SPI1_TX`, so only one transfer may be in flight —
starting a second one would overwrite the first one's registers. With one
band (4 KB) taking 0.82 ms on the wire and 8 lines taking ~1.8 ms to
emulate, the transfer hides completely behind the emulation.

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

**A** is the blue button (PC13), **B** is the blue button together with
down, **START** is the blue button together with up — five switches have
to cover the NES's eight. The NES's own protocol (`$4016` strobe + shift)
is emulated in `nes.c`, so the cartridge sees a normal controller.

## Testing strategy

Three levels, all reproducible from the repository:

| Level | Command | What it proves |
|---|---|---|
| CPU unit test | `make host-test` | 19 checks on the 6502 core, run on the PC with a flat 64 KB bus |
| Emulator on the PC | `make host-rom` | renders frames to a PNG using the same `ppu.c`/`nes.c` as the board |
| Hardware | `make flash` + a framebuffer dump | the board's picture matches the PC reference frame |

The third one is the important one: the same cartridge is run on both
sides and the framebuffer is compared pixel by pixel. A one-frame
misalignment already shows up as ~4% of pixels differing, so the test is
sharp — a correct match is a few hundred pixels (animated sprites that
depend on the exact moment of the OAM DMA).
