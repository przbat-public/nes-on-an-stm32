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
- OpenOCD (optional, for framebuffer dumps): `brew install openocd`
- Python 3 for the ROM generator and the host test rigs

## Build and flash

```bash
make            # firmware with the self-test cartridge embedded
make flash      # st-flash write + reset
```

The cartridge is generated and embedded automatically: `tools/make_test_rom.py`
builds `build/test.nes` (own font, own artwork), `tools/rom2c.py` turns it
into `src/rom_data.c`, and the linker puts it in flash next to the code.

To use a different cartridge, drop an NROM (mapper 0) `.nes` image
somewhere and point the build at it:

```bash
make ROM=~/roms/some-nrom-game.nes flash
```

## Tests

```bash
make host-test    # 6502 core unit tests (19 checks), run on the PC
make host-rom     # run the emulator on the PC, render frames to a PNG
make rom          # just regenerate the self-test cartridge
```

`make host-rom` writes `build/frame.raw` (256×240 NES colour indices)
and converts it to `build/frame.png` with the NES palette — useful to see
what the emulator *should* be showing.

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

A locked frame shows a few hundred differing pixels (animated sprites
depend on the exact moment of the OAM DMA); a one-frame misalignment
already shows ~2500, i.e. ~4%.

## Reloading without a debugger

There is no ROM loader yet — cartridges are baked into the firmware at
build time. A serial (XMODEM over the ST-LINK virtual COM port) loader is
the obvious next step.
