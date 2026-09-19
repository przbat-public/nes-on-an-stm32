# Build & flash

## Hardware

| Part | Role |
|---|---|
| NUCLEO-L476RG | the board: STM32L476RG, Cortex-M4 at 80 MHz, 1 MB flash, 128 KB RAM |
| X-NUCLEO-GFX01M2 | display shield: 240×320 ST7789 LCD over SPI, joystick |

Mount the shield with the joystick on the left of the USB connector
(rotated the other way it covers the debug pins).

The emulator drives the panel **in landscape**, so hold the board with
the USB sockets to the left; the 256×240 picture is centred with black
bars on the sides. That is a quarter turn from the upright (portrait)
hold, and the joystick turns with the board — which is why the pin map in
`input.c` is the upright one rotated 90° (see
[docs/ARCHITECTURE.md](ARCHITECTURE.md#the-pad-inputc)).

## Toolchain

- Arm cross-compiler: `brew install --cask gcc-arm-embedded` (macOS) or
  `sudo apt install gcc-arm-none-eabi`
- stlink tools: `brew install stlink` or `sudo apt install stlink-tools`
- OpenOCD: `brew install openocd` — needed for framebuffer dumps, for the
  live counters (`tools/swd.py`) and for flashing while it holds the ST-Link
- Python 3 for the ROM generator, the host test rigs and `tools/swd.py`

## Build and flash

```bash
make            # firmware with the self-test cartridge embedded
make flash      # st-flash write + reset
```

The cartridge is generated and embedded automatically: `tools/make_test_rom.py`
builds `build/test.nes` (own font, own artwork), `tools/rom2c.py` turns it
into `src/rom_data.c`, and the linker puts it in flash next to the code.

To use a different cartridge, drop a `.nes` image somewhere and point the
build at it:

```bash
make ROM=~/roms/some-game.nes flash
```

`ROM=` accepts anything the emulator implements: **NROM** (mapper 0),
**MMC1** (1), **UxROM** (2) and **MMC3** (4), with CHR ROM or CHR RAM.

## Pixel formats

The panel is driven in **RGB565** by default. `make LCD_12BIT=1` builds the
same firmware for the ST7789's **RGB444** mode: two pixels in three bytes,
25% less traffic on the SPI wire, 4 bits per channel instead of 5/6/5. It is
not faster on this board yet — the emulation is still the long pole — so the
16-bit path stays the default; the numbers are in
[docs/PERFORMANCE.md](PERFORMANCE.md).

```bash
make LCD_12BIT=1          # RGB444 firmware
make LCD_12BIT=1 flash
```

## Tests

```bash
make host-test      # 6502 core unit tests (19 checks), run on the PC
make host-ppu-test  # background expansion tables vs the old loop, exhaustively
make host-rom       # run the emulator on the PC, render frames to a PNG
make rom            # just regenerate the self-test cartridge
```

`make host-rom` writes `build/frame.raw` (256×240 NES colour indices) and
converts it to `build/frame.png` with the NES palette — useful to see what the
emulator *should* be showing. It also prints a checksum of the frame and the
cartridge's own result bytes from RAM `$0300`-`$030F`, which is how the board
and the PC are compared.

The same generator makes the other two self-test cartridges; each leaves its
verdict in console RAM, and `$030F` = `3F` means all six MMC3 checks passed:

```bash
python3 tools/make_test_rom.py --mmc1   # build/mmc1.nes
python3 tools/make_test_rom.py --mmc3   # build/mmc3.nes
make ROM=build/mmc3.nes flash
```

`tools/mmc3_result.c` runs that cartridge on the host and prints the byte
(the compile line is in the file's header); on the board, read `$030F` out of
console RAM over SWD.

For any change to the 6502 core, the differential harness builds the core from
a git revision and runs random instruction streams through both it and the
working tree's core, comparing state hashes:

```bash
tools/diff_run.sh 5000000 HEAD      # 5 M instructions, must be identical
```

## Verifying the board

The sharpest test is comparing the board's framebuffer with a PC render
of the same frame. With OpenOCD attached:

```bash
FB=$(grep -E "^\s+\.bss\.fb\s" emu.map | awk '{print $2}')   # from the fresh map
# in the openocd console:
halt
mdw 0x20000010 1                 # dbg_frames: which frame the board is on
dump_image frame.raw $FB 61440
```

Then render the same frame number on the PC and compare:

```bash
./build/host_render build/test.nes 674 host.raw
python3 - <<'EOF'
hw = open('frame.raw','rb').read(); ho = open('host.raw','rb').read()
print(sum(1 for a,b in zip(hw,ho) if a != b), "differing pixels of", len(hw))
EOF
```

A locked frame shows **0 differing bytes** of 61,440 — that is the pass mark
every cartridge in the README's table meets — and a one-frame misalignment
already shows ~2500 differing pixels, i.e. ~4%.

The firmware can also be interrogated while it runs, which is usually faster
than dumping a frame. `tools/swd.py` reads the counters and the boot
self-checks over OpenOCD's telnet port:

```bash
python3 tools/swd.py read           # one sample
python3 tools/swd.py budget 5       # per-frame breakdown over a 5 s window
python3 tools/swd.py track 300 600  # the same counters at given emulated frames
python3 tools/swd.py stats 10 1     # mean/min/max over ten samples
python3 tools/swd.py watch 30       # a sample every 2 s
```

Among the counters are the display path's own boot self-checks:
`dbg_lcd_conv_ok` (the colour conversion matches bytes worked out by hand),
`dbg_lcd_band_ok` (the fast band-skip decision agrees with an obvious
byte-wise one), `dbg_lcd_invariant_ok` (every skipped band still matches what
the panel was last given) and `dbg_lcd_readback_ok` (0 on this shield, whose
panel SDO is not wired back).

Flashing while OpenOCD is attached needs `tools/ocd_flash.sh`: `st-flash`
cannot open the ST-Link that OpenOCD holds, and with its output piped it
fails quietly rather than loudly.

```bash
tools/ocd_flash.sh emu.bin          # reset halt, write, verify, reset run
```

## Reloading without a debugger

There is no ROM loader yet — cartridges are baked into the firmware at
build time. A serial (XMODEM over the ST-LINK virtual COM port) loader is
the obvious next step.

## The tools, and which of them need the board

Host only, they run wherever the sources build:

| Tool | What it does |
|---|---|
| `tools/host_test.c` (`make host-test`) | 19 checks on the 6502 core |
| `tools/host_render.c` (`make host-rom`) | renders a cartridge's frames on the PC |
| `tools/ppu_expand_test.c` (`make host-ppu-test`) | 262,144 checks on the background expansion tables |
| `tools/ppu_nt_test.c` (`make host-nt-test`) | 1,966,080 checks on the nametable walk |
| `tools/diff_run.sh` | random instruction streams through two cores, state compared instruction by instruction |
| `tools/mmc3_result.c` | runs the MMC3 self-test cartridge and prints its result byte |
| `tools/make_test_rom.py` (`--mmc1`, `--mmc3`) | builds the self-test cartridges |
| `tools/gen_6502.py` | generates `src/cpu_ops.h`; that file is never edited by hand |
| `tools/asm6502.py`, `tools/cpu_test.py` | the small assembler and the test program behind `host-test` |
| `tools/rom2c.py` | turns a `.nes` file into the C array the firmware embeds |
| `tools/raw2png.py` | turns a frame dump into a PNG using the NES palette |

Needs the board and a debugger attached:

| Tool | What it does |
|---|---|
| `tools/swd.py` | reads the `.bss` counters over SWD: `read`, `budget SECS`, `track FRAMES...`, `stats`, `watch` |
| `tools/board_run.py` | drives the board with a pad script, so board frame N can be compared with host frame N |
| `tools/ocd_flash.sh` | flashes through openocd; `st-flash` silently does nothing while openocd holds the ST-Link |

Two habits these tools exist for. Nothing about the hardware counts as verified until the
board's framebuffer has been compared with the host's frame for the same emulated frame.
Nothing about the emulation counts as verified until the self-test cartridges render
byte-identically before and after the change.
