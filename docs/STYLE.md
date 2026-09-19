# Coding standard

This is the standard for everything in `src/` and `tools/`. It describes what the code
already does well, so that new code matches it, and it names the places where this
codebase is deliberately unusual — so nobody "fixes" them by accident.

## What shapes every decision

The target is an STM32L476RG: Cortex-M4F at 80 MHz, 96 KB of SRAM, no operating system.
The emulator must run a 1.79 MHz 6502 and draw 256x240 pixels through a serial panel,
and it does that at tens of frames per second. Two budgets are therefore always in play:

- **RAM.** Every buffer is a fixed-size static, sized at compile time. Do not introduce
  dynamic allocation: there is no heap. The framebuffer alone is 60 KB, so before adding
  any buffer, say what it costs and where the space comes from.
- **Time.** A frame is measured, not guessed. `tools/swd.py` reads the per-frame counters
  over SWD (`dbg_cyc_frame`, `dbg_cyc_cpu`, `dbg_cyc_ppu`, `dbg_cyc_hook`), and
  `make host-test`, `make host-ppu-test` and `make host-nt-test` prove the emulation did
  not change. A change to a hot path without a before/after number is not finished.

## Files and layers

Each module owns one job and states its contract at the top of its header. The layers,
from the hardware upwards:

| File | Owns |
|---|---|
| `hal.c` / `hal.h` | every hardware register in the project. Nothing else writes to a register address. |
| `lcd.c` | the panel: init, window commands, band staging, the pixel conversion, and its own boot self-checks. |
| `input.c` | the switches, and the mapping from five physical switches to the NES pad byte. |
| `cpu6502.c` | the emulated processor. It knows nothing about the NES: all memory goes through `bus_read`/`bus_write`. |
| `ppu.c` | the picture chip: scanline renderer, sprites, scrolling, status flags. |
| `mapper.c` | cartridge banking: NROM, MMC1, UxROM, MMC3, MMC5. |
| `nes.c` | the machine: memory map, cartridge loading, controller protocol, frame loop. |
| `nesmem.h` | the inline fast paths for RAM and cartridge ROM, published as bank pointers. |

Two consequences worth stating: `cpu6502.c` must stay portable to the PC (that is how bugs
are reproduced), and `nesmem.h` exists because a function call per memory access cost more
than the emulation itself.

## Naming

- `snake_case` for functions and variables, `UPPER_CASE` for macros and constants,
  `PORT_B`-style names for hardware ports. Types end in `_t`.
- A name says what the thing *is*, not how it is implemented: `band_bytes`, not `buf2`.
- Registers and line numbers from the NES documentation keep their names (`ctrl`, `mask`,
  `status`, `v`, `t`, `fine_x`), because a reader will meet those names in the reference
  material. Emulator-invented concepts get descriptive names (`line_h`, `held`, `sent`).

## Constants

- No bare hex addresses in logic. Register bases and bit masks live in `hal.h` or as
  named constants next to the code that uses them.
- NES constants that come from the hardware documentation (`0x2000`, `0x4016`, `113.67`
  cycles per line) may appear literally **once**, in the module that owns that piece of
  the machine, with a comment naming the source.

## Functions

- One job per function. If a function needs a paragraph to describe what it does, split it.
- Prefer early returns over nested conditionals; the frame loop reads top to bottom.
- Pass what the function needs; do not reach for globals that the caller could have passed,
  except for the module's own state.
- `static` everything that is not part of the module's contract. The headers show the API;
  anything not in a header is private by definition.

## Comments

The code says *what* happens. Comments say *why*, and what would break otherwise.

- Every module starts with a header comment: what it owns, what it assumes about the
  layer below, and any hardware quirk that shaped it.
- Explain the reason for anything that looks odd. If a line exists because of a timing
  measurement, a hardware erratum, or a real cartridge's behaviour, say which.
- Do not narrate the obvious (`i++ /* increment i */`). Do document the non-obvious:
  why a flag is cleared here and not there, why a buffer is 4 KB and not 8 KB.
- When a fix came from a bug, name the symptom it prevents. Those comments are the only
  thing standing between a future reader and the same bug.

## Deliberately unusual, do not "clean up"

- **`cpu_run()` holds the 6502 registers in automatic variables and reaches them through
  macros.** File-scope statics cannot stay in registers across a bus access, which cost
  two days to learn; the macros are why the core is 19% faster on the target.
- **`src/cpu_ops.h` is generated** by `tools/gen_6502.py`. Never edit it by hand, and any
  change to the generated shapes goes in the generator.
- **The PPU renders straight into the display framebuffer** when the mapper allows it. The
  extra copy that this removes cost 2 ms a frame.
- **Bands are compared against `held[]`, never against a single previous band.** A skip
  decision that compares against the wrong reference leaves stale stripes on the panel.
- **The `.bss` counters are an interface.** They are read over SWD while the game runs, by
  `tools/swd.py` and by the board tests; renaming or moving them breaks that tooling.
- **The self-checks in `lcd.c` are not paranoia.** A wrong byte order in the pixel
  conversion or a broken band comparison shows up only as wrong colours or stale stripes
  on a panel nobody is looking at programmatically.

## Tests are the contract

Any change to emulation behaviour must keep these green, and the numbers belong in the
commit message:

| Command | Proves |
|---|---|
| `make host-test` | 19 checks on the 6502 core |
| `make host-ppu-test` | 262,144 checks on the background expansion tables |
| `make host-nt-test` | 1,966,080 checks on the nametable walk, all mappings and scrolls |
| `make host-rom ROM=... FRAMES=...` | a frame rendered by the same core on the PC |
| `tools/diff_run.sh N HEAD` | random instruction streams against a core from a git revision |
| `tools/swd.py track` | frame time on the board, at fixed emulated frames |

The three self-test cartridges (`make rom`, `--mmc1`, `--mmc3`) render pixel-identically
before and after any change; that comparison, not reviewer confidence, is what decides.

## The plan this standard serves

The repository is being made readable enough to teach from: a Polish tutorial for an
electronics technical-school student is being written in `tutorial-pl/`, with its own
staged, simplified code. The refactor proceeds in increments, each one landing with the
guards green:

1. **Style and the safe modules** (this document, plus `hal.c`, `input.c`, `font5x7.c`,
   `main.c`, headers). Low risk: no hot path, no buffer changes.
2. **Readability of the machine layer** (`nes.c`, `nesmem.h`): naming, smaller functions,
   contract comments. Medium risk: the inline fast paths must keep their shape, verified
   by the frame comparisons and the board's frame time.
3. **The emulation cores** (`cpu6502.c`, `ppu.c`, `mapper.c`): comments and naming only,
   no restructuring. High risk, so each file lands on its own with the full guard set,
   including `tools/diff_run.sh` for the core.
4. **The tools** (`tools/*.py`, `tools/*.c`): one-off scripts to keep, one-off scripts to
   delete. Low risk, but the test tools must stay runnable from a clean checkout.
