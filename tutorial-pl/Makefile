# Build and flash one stage of the tutorial:
#
#   make STAGE=00            build it
#   make STAGE=00 flash      build it and write it to the board
#   make STAGE=01 clean      remove the build products
#
# The stages deliberately borrow the emulator's linker script and startup file:
# the point of the first stages is the panel's protocol, not the vector table,
# and stage 2 explains what those two files do when interrupts arrive.

STAGE   ?= 00
STAGE_DIR = etap-$(STAGE)

CC       = arm-none-eabi-gcc
OBJCOPY  = arm-none-eabi-objcopy
SIZE     = arm-none-eabi-size

# The linker script sits one directory above, in the emulator's sources.
CFLAGS   = -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
           -T ../src/linker.ld
SOURCES  = $(STAGE_DIR)/main.c ../src/startup_l476.s

TARGET   = $(STAGE_DIR)/etap$(STAGE)

.PHONY: all flash clean check

all: $(TARGET).bin

$(TARGET).elf: $(SOURCES) ../src/linker.ld
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
