# Stage 7 is a program for the computer, not for the board: it reads a
# cartridge file from disk. So it is built with the host compiler instead of
# the cross compiler, and there is nothing to flash.
#
#   make            build it
#   make run        build it and read the emulator's test cartridge
#   make clean      remove the build products
#
# `make STAGE=07 check` from the directory above does not apply here: that
# Makefile builds every stage with the Arm cross compiler, and this program
# needs the host's C library. Run `make check` in this directory instead.

CC     ?= cc
CFLAGS  = -Wall -Wextra
TARGET  = etap07

.PHONY: all run check clean

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ main.c

run: $(TARGET)
	./$(TARGET) ../../build/test.nes

check:
	$(CC) $(CFLAGS) -o /dev/null main.c

clean:
	rm -f $(TARGET)
