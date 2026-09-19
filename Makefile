# NES emulator for NUCLEO-L476RG + X-NUCLEO-GFX01M2
#
#   make            firmware binary (emu.bin) with the test ROM embedded
#   make flash      write it to the board and reset
#   make rom        build the self-test cartridge (build/test.nes)
#   make host-test  run the 6502 core tests on the PC
#   make host-rom   run the emulator on the PC and dump a frame
#   make clean

TARGET  = emu
ROM     ?= build/test.nes
FRAMES  ?= 60

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O2 -g \
          -ffunction-sections -fdata-sections -Wall -Wextra -Isrc \
          -DNES_BUS_INLINE -DNES_PROFILING

SRCS    = src/main.c src/hal.c src/lcd.c src/input.c \
          src/cpu6502.c src/ppu.c src/nes.c src/mapper.c \
          src/font5x7.c src/rom_data.c
OBJS    = $(SRCS:.c=.o) src/startup_l476.o

HOSTCC  ?= cc
HOSTCFLAGS = -O2 -Wall -Wextra -Isrc -Ibuild
HOST_SRCS  = src/cpu6502.c src/ppu.c src/nes.c src/mapper.c

.PHONY: all flash rom romdata host-test host-rom clean

all: $(TARGET).bin

# ---- cartridge ---------------------------------------------------------
rom:
	python3 tools/make_test_rom.py

# always regenerate the embedded cartridge (it is what the ROM variable
# points at); 'rom' rebuilds the self-test image itself
romdata: rom
	python3 tools/rom2c.py $(ROM) src/rom_data.c

# ---- firmware ----------------------------------------------------------
$(TARGET).elf: romdata $(OBJS) src/linker.ld
	$(CC) $(CFLAGS) -T src/linker.ld -nostartfiles -Wl,--gc-sections \
	      -Wl,-Map=$(TARGET).map -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/startup_l476.o: src/startup_l476.s
	$(CC) $(CFLAGS) -c -o $@ $<

flash: $(TARGET).bin
	st-flash write $(TARGET).bin 0x08000000
	st-flash reset

# ---- host tools (no hardware needed) -----------------------------------
host-test: build/cpu_test.h
	$(HOSTCC) $(HOSTCFLAGS) -o build/host_test tools/host_test.c src/cpu6502.c
	./build/host_test

build/cpu_test.h: tools/cpu_test.py tools/asm6502.py tools/gen_6502.py
	python3 tools/cpu_test.py

host-rom: build/host_render $(ROM)
	./build/host_render $(ROM) $(FRAMES) build/frame
	python3 tools/raw2png.py build/frame.raw build/frame.png 2

build/host_render: tools/host_render.c $(HOST_SRCS) src/cpu_ops.h
	$(HOSTCC) $(HOSTCFLAGS) -DNES_BUS_INLINE -o $@ tools/host_render.c $(HOST_SRCS)

src/cpu_ops.h: tools/gen_6502.py
	python3 tools/gen_6502.py > src/cpu_ops.h

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).map src/rom_data.c
	rm -rf build
