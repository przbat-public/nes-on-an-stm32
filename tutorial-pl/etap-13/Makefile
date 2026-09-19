# Stage 13 has three source files, and the shared Makefile one directory up
# builds exactly one (etap-NN/main.c), so this stage keeps its own copy of the
# same rules. The flags and the borrowed files are identical on purpose: this
# is the same build, spelled out for a stage that needs more than one file.
#
# Run it from this directory:
#
#   make            build the image the board runs
#   make check      compile it and stop, for a machine with no board
#   make flash      write it to the board and reset it
#   make clean      remove the build products
#
# From the tutorial-pl directory the same thing is
#   make -C etap-13 check
#
# The chapter also shows the compiler command in full, so you can run it
# without make at all.

STAGE ?= 13

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# The linker script and the startup file sit in the emulator's sources, which
# are two directories up from here.
EMU     ?= ../../src
CFLAGS   = -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
           -T $(EMU)/linker.ld
SOURCES  = main.c cpu.c ppu.c $(EMU)/startup_l476.s

TARGET   = etap$(STAGE)

.PHONY: all check flash clean

all: $(TARGET).bin

$(TARGET).elf: $(SOURCES) $(EMU)/linker.ld
	$(CC) $(CFLAGS) -o $@ $(SOURCES)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

# st-flash writes the image but does not reset the chip, so the second command
# is not optional: without it the board keeps running whatever it ran before.
flash: $(TARGET).bin
	st-flash write $(TARGET).bin 0x08000000
	st-flash reset

# What the chapter promises: the stage must build without a single warning.
check:
	$(CC) $(CFLAGS) -o /dev/null $(SOURCES)

clean:
	rm -f $(TARGET).elf $(TARGET).bin
