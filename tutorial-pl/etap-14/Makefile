# Stage 14 is a program for the computer, not for the board: it builds the
# cartridge chip and runs a program on it, which is the only way to see whether
# a chip works. So it is built with the host compiler, and there is nothing to
# flash.
#
#   make            build it
#   make run        build it and write the picture
#   make check      build it, run it, and fail if a check fails
#   make clean      remove the build products
#
# The shared Makefile above this directory builds the stages that run on the
# board; this stage and the ones around it are host programs.

CC      ?= cc
CFLAGS   = -Wall -Wextra
TARGET   = etap14
SOURCES  = main.c cpu.c ppu.c mmc5.c
HEADERS  = cpu.h ppu.h mmc5.h
LDLIBS   = -lz

.PHONY: all run check clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

check: $(TARGET)
	./$(TARGET)
	@echo "all checks passed"

clean:
	rm -f $(TARGET) etap14.png
