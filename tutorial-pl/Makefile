# Build and run one stage of the tutorial:
#
#   make STAGE=00            build it
#   make STAGE=00 flash      build it and write it to the board
#   make STAGE=00 check      check it compiles without a single warning
#   make STAGE=00 run        run it (stages for the computer only)
#   make STAGE=01 clean      remove the build products
#
# Stages come in three kinds:
#
#   board   a program for the microcontroller: built with the Arm compiler and
#           the emulator's linker script and startup file. Its result is a
#           picture on the panel, so `flash` makes sense for it.
#   host    a program for the computer, used where the thing being taught (the
#           processor, the cartridge header, measurements) is easier to see on
#           a screen than on a panel. Its result is text, so `run` makes sense.
#   own     the stage brings its own Makefile because it needs more than one
#           source file; this file hands over to it.
#
# The stages that were written first borrowed the emulator's startup file
# because the point was the panel's protocol, not the vector table; stage 10
# explains what that file does once interrupts arrive.

STAGE     ?= 00
STAGE_DIR  = etap-$(STAGE)

# Which stages are programs for the computer rather than for the board. Kept
# here, in one list, so a reader can see at a glance how the two kinds differ.
HOST_STAGES = 05 06 07 12 14 15
HOST_LIBS   = -lz

ifeq ($(wildcard $(STAGE_DIR)/Makefile),)
  ifeq ($(filter $(STAGE),$(HOST_STAGES)),)
    KIND    = board
    CC      = arm-none-eabi-gcc
    OBJCOPY = arm-none-eabi-objcopy
    SIZE    = arm-none-eabi-size
    CFLAGS  = -mcpu=cortex-m4 -mthumb -O2 -Wall -Wextra -nostartfiles \
              -T ../src/linker.ld
    SOURCES = $(STAGE_DIR)/main.c ../src/startup_l476.s
  else
    KIND    = host
    CC      = cc
    CFLAGS  = -O2 -Wall -Wextra
    SOURCES = $(STAGE_DIR)/main.c
    LIBS    = $(HOST_LIBS)
  endif
else
  KIND      = own
endif

TARGET = $(STAGE_DIR)/etap$(STAGE)

.PHONY: all flash run check clean

ifeq ($(KIND),own)

all flash run check clean:
	$(MAKE) -C $(STAGE_DIR) $@

else ifeq ($(KIND),host)

all: $(TARGET)
	@echo "this stage runs on the computer: make STAGE=$(STAGE) run"

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LIBS)

run: $(TARGET)
	./$(TARGET)

check:
	$(CC) $(CFLAGS) -o /dev/null $(SOURCES) $(LIBS)
	@echo "stage $(STAGE): compiles without warnings (program for the computer)"

flash:
	@echo "stage $(STAGE) is a program for the computer, not for the board."
	@echo "Use: make STAGE=$(STAGE) run"
	@false

clean:
	rm -f $(TARGET)

else

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

check:
	$(CC) $(CFLAGS) -o /dev/null $(SOURCES)
	@echo "stage $(STAGE): compiles without warnings (program for the board)"

run:
	@echo "stage $(STAGE) is a program for the board: make STAGE=$(STAGE) flash"
	@false

clean:
	rm -f $(TARGET).elf $(TARGET).bin

endif
