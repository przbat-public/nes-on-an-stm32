#!/usr/bin/env python3
"""Build tool for stage 13 — not part of the lesson.

There is no cross-assembler in this repository, so this script is what turned
the demo program into the byte arrays that cart.c carries. It stays next to
the stage so the numbers in the C file can be regenerated and checked.

    python3 asm.py            # the three byte arrays, as C source
    python3 asm.py rom.bin    # the same thing as one 48 KB image
    python3 asm.py --listing  # the program as text, for reading
"""

import sys

ORIGIN_BANK = 0x8000
ORIGIN_FIXED = 0xC000
BANK_SIZE = 0x4000

# ---------------------------------------------------------------- opcodes
# Only what the demo program uses. `zp` is one byte of address, `abs` two.
OPC = {
    "lda": {"imm": 0xA9, "zp": 0xA5, "abs": 0xAD, "abx": 0xBD, "iny": 0xB1},
    "ldx": {"imm": 0xA2, "zp": 0xA6, "abs": 0xAE},
    "ldy": {"imm": 0xA0, "zp": 0xA4},
    "sta": {"zp": 0x85, "abs": 0x8D, "iny": 0x91},
    "tax": {"imp": 0xAA}, "txa": {"imp": 0x8A}, "txs": {"imp": 0x9A},
    "inx": {"imp": 0xE8}, "iny": {"imp": 0xC8}, "dex": {"imp": 0xCA},
    "dey": {"imp": 0x88},
    "adc": {"imm": 0x69, "zp": 0x65},
    "and": {"imm": 0x29, "zp": 0x25},
    "eor": {"imm": 0x49},
    "cmp": {"imm": 0xC9},
    "cpx": {"imm": 0xE0},
    "bne": {"rel": 0xD0}, "beq": {"rel": 0xF0}, "bpl": {"rel": 0x10},
    "clc": {"imp": 0x18}, "cld": {"imp": 0xD8}, "sei": {"imp": 0x78},
    "bit": {"zp": 0x24, "abs": 0x2C},
    "inc": {"zp": 0xE6, "abs": 0xEE},
    "jmp": {"abs": 0x4C, "ind": 0x6C},
    "jsr": {"abs": 0x20}, "rts": {"imp": 0x60},
    "nop": {"imp": 0xEA},
}

# ------------------------------------------------------------- the program
# The names the two halves of the program agree on. These are the console's
# addresses, not the board's: what the emulated 6502 sees.
ZP_LO, ZP_HI = 0x00, 0x01        # the 16-bit pointer the copy loop walks
VAR_FRAME = 0x10                 # low byte of the frame counter
VAR_FRAME_HI = 0x11              # high byte: counts 256 frames per step

# The bank register is not memory the program can read back: it is the
# cartridge listening on the bus. Any write anywhere in the cartridge's range
# reaches it, so the game uses an address in the fixed half, where there is
# nothing else to disturb.
BANK_REG = 0xC000



PPU_CTRL, PPU_MASK, PPU_STATUS = 0x2000, 0x2001, 0x2002
PPU_SCROLL, PPU_ADDR, PPU_DATA = 0x2005, 0x2006, 0x2007

CHR_AT = 0xC300                  # the tile shapes, in the fixed bank
PAL_AT = 0xFE00                  # the palette, in the fixed bank
ROWS = 30                        # nametable rows on the screen
COLS = 32                        # nametable columns
ROWS_PER_BANK = ROWS // 2


class Asm:
    def __init__(self, origin):
        self.org = origin
        self.pc = origin
        self.out = bytearray()
        self.labels = {}
        self.fixups = []                 # (offset, name, kind)
        self.listing = []                # (offset, length, text) of each line
        self.notes = []                  # where a table starts, for the listing

    def label(self, name):
        self.labels[name] = self.pc

    def emit(self, b):
        self.out.append(b & 0xFF)

    def emit16(self, v):
        self.emit(v)
        self.emit(v >> 8)

    def ins(self, text):
        """Assemble one line: "lda #$00", "sta $2007", "lda TABLE,x"."""
        start = len(self.out)
        mnem = text.split()[0]
        operand = text[len(mnem):].strip()
        table = OPC[mnem]

        if not operand:
            self.emit(table["imp"])
            self.pc += 1
        elif operand.startswith("#"):
            self.emit(table["imm"])
            self.fixups.append((len(self.out), operand[1:], "imm"))
            self.emit(0)
            self.pc += 2
        elif operand.endswith(",x"):
            self.emit(table["abx"])
            self.fixups.append((len(self.out), operand[:-2], "abs"))
            self.emit16(0)
            self.pc += 3
        elif operand.startswith("("):
            inner = operand[1:operand.index(")")]
            self.emit(table["inx"] if operand.endswith("x)") else table["iny"])
            self.fixups.append((len(self.out), inner, "zp"))
            self.emit(0)
            self.pc += 2
        else:
            value = self.labels.get(operand)
            if value is None:
                value = lookup(operand)
            if value is None:
                # a label defined further down: a jump or a call, so two
                # bytes of address
                self.emit(table["abs"])
                self.fixups.append((len(self.out), operand, "abs"))
                self.emit16(0)
                self.pc += 3
            else:
                self.emit_zp_or_abs(table, value)
                self.pc += 2 if value <= 0xFF else 3

        self.listing.append((start, len(self.out) - start, text))

    def emit_zp_or_abs(self, table, value):
        if value <= 0xFF:
            self.emit(table["zp"])
            self.emit(value)
        else:
            self.emit(table["abs"])
            self.emit16(value)

    def branch(self, mnem, label):
        start = len(self.out)
        self.emit(OPC[mnem]["rel"])
        self.fixups.append((len(self.out), label, "rel"))
        self.emit(0)
        self.pc += 2
        self.listing.append((start, len(self.out) - start, mnem + " " + label))

    def note(self, at, length, text):
        """A region for the C listing: where it starts and how long it is."""
        self.notes.append((at - self.org, length, text))

    def raw(self, data, note=""):
        start = len(self.out)
        for b in data:
            self.emit(b)
        self.pc += len(data)
        self.notes.append((start, len(data), note))

    def resolve(self):
        for off, name, kind in self.fixups:
            value = self.labels.get(name)
            if value is None:
                value = lookup(name)
            if value is None:
                raise ValueError("undefined label " + name)
            if kind == "rel":
                delta = value - (self.org + off + 1)
                if not -128 <= delta <= 127:
                    raise ValueError("branch out of range to " + name)
                self.out[off] = delta & 0xFF
            elif kind == "imm" or kind == "zp":
                self.out[off] = value & 0xFF
            else:
                self.out[off] = value & 0xFF
                self.out[off + 1] = (value >> 8) & 0xFF
        return bytes(self.out)


def lookup(text):
    """A number, or one of the names the program is written against.

    Returns None for anything else, which is how a label defined further down
    the listing is recognised: it gets a two-byte fixup and is patched once
    every label is known.
    """
    names = {"ZP_LO": ZP_LO, "ZP_HI": ZP_HI, 
             "VAR_FRAME": VAR_FRAME, "VAR_FRAME_HI": VAR_FRAME_HI,
             "BANK_REG": BANK_REG,
             "PPU_CTRL": PPU_CTRL, "PPU_MASK": PPU_MASK,
             "PPU_STATUS": PPU_STATUS, "PPU_SCROLL": PPU_SCROLL,
             "PPU_ADDR": PPU_ADDR, "PPU_DATA": PPU_DATA,
             "CHR_TILES": CHR_AT, "PALETTE": PAL_AT,
             # the one entry point the fixed bank calls in the other bank:
             # it sits at the very start of $8000, in both of them
             "BANK_DRAW": ORIGIN_BANK}
    text = text.strip()
    if text in names:
        return names[text]
    if text.startswith("$"):
        return int(text[1:], 16)
    if text.startswith("%"):
        return int(text[1:], 2)
    if text.startswith("0x"):
        return int(text, 16)
    try:
        return int(text, 10)
    except ValueError:
        return None


# ------------------------------------------------- the switchable banks
def build_bank(bank):
    """One switchable bank: the code that draws its half of the screen.

    The two banks hold the same routine, byte for byte, except for one thing
    at the end: the colour it starts from. Bank 0 owns rows 0-14 of the
    picture and starts at colour 1; bank 1 owns rows 15-29 and starts at
    colour 3. The routine does not run from $C000, where the rest of the
    program lives — it runs from $8000, the window the bank register
    controls, and in this cartrige it is the only thing in that window.

    The colour it writes grows by one for every row, so the half it owns is a
    staircase of nine shades of one colour; each time the game calls it, the
    staircase starts one step further along and the whole half slides through
    the palette. Two banks, two colours, and both visibly running.
    """
    a = Asm(ORIGIN_BANK)
    first_row = bank * ROWS_PER_BANK
    ppu_at = 0x2000 + first_row * COLS
    first_colour = 2 + bank * 2

    a.label("BANK_DRAW")
    # Two jobs, and between them they are how a picture is made: fill the
    # grid with tile numbers, then say what colour those tiles are.
    a.ins("lda #${0:02X}".format(ppu_at >> 8))       # where in video memory to
    a.ins("sta PPU_ADDR")                            # start writing, high
    a.ins("lda #${0:02X}".format(ppu_at & 0xFF))     # byte of the address first
    a.ins("sta PPU_ADDR")

    # Bank 0 fills its half with tile 0, bank 1 with tile 1. The two tiles
    # hold different pixel values, so the halves are made of different stuff
    # and not merely painted differently.
    a.ins("lda #" + str(bank))
    a.ins("ldx #" + str(ROWS_PER_BANK))              # fifteen rows of
    a.label("row")
    a.ins("ldy #" + str(COLS))                       # thirty-two tiles
    a.label("cell")
    a.ins("sta PPU_DATA")                            # the chip walks the
    a.ins("dey")                                     # address on by itself,
    a.branch("bne", "cell")                          # so the loop only counts
    a.ins("dex")
    a.branch("bne", "row")

    # Then the colour of this half, into its own palette entry. Reading the
    # bank register is the one place the program asks the cartridge which
    # piece of it is in the window, and the answer is what the picture shows.
    a.ins("lda #${0:02X}".format((0x3F00 + 1 + bank) >> 8))
    a.ins("sta PPU_ADDR")
    a.ins("lda #${0:02X}".format((0x3F00 + 1 + bank) & 0xFF))
    a.ins("sta PPU_ADDR")
    a.ins("lda BANK_REG")                            # 0 in bank 0, 1 in bank 1
    a.ins("clc")
    a.ins("adc VAR_FRAME")                           # plus the game's clock
    a.ins("adc #" + str(first_colour + 4))           # plus a number of its own,
    a.ins("and #$0E")                                # so the two halves change
    a.ins("sta PPU_DATA")                            # colour as the game runs
    a.ins("rts")

    a.note(ORIGIN_BANK, a.pc - ORIGIN_BANK,
           "BANK_DRAW: this bank's routine, at the window's first byte")

    code = a.resolve()
    if len(code) >= BANK_SIZE:
        raise ValueError("bank %d does not fit" % bank)
    image = bytearray(BANK_SIZE)
    image[:len(code)] = code
    return bytes(image), a


# ------------------------------------------------------ the fixed bank
def build_fixed():
    """The last bank: reset, the two tables, and the loop that runs the game.

    The vectors live at the top of this bank, so this bank can never leave
    $C000-$FFFF: the processor has to find the reset vector the moment it
    starts. Every bank-switching cartridge keeps one bank like that, which is
    also why the routine that changes the bank can live in it.
    """
    a = Asm(ORIGIN_FIXED)

    a.label("RESET")
    a.ins("sei")
    a.ins("cld")
    a.ins("ldx #$FF")
    a.ins("txs")

    # ---- the tile shapes, into video memory ----
    a.ins("lda #$00")
    a.ins("sta PPU_ADDR")
    a.ins("lda #$00")
    a.ins("sta PPU_ADDR")
    a.ins("ldx #0")
    a.label("chr")
    a.ins("lda CHR_TILES,x")
    a.ins("sta PPU_DATA")
    a.ins("inx")
    a.ins("cpx #64")
    a.branch("bne", "chr")

    # ---- the palette ----
    a.ins("lda #$3F")
    a.ins("sta PPU_ADDR")
    a.ins("lda #$00")
    a.ins("sta PPU_ADDR")
    a.ins("ldx #0")
    a.label("pal")
    a.ins("lda PALETTE,x")
    a.ins("sta PPU_DATA")
    a.ins("inx")
    a.ins("cpx #32")
    a.branch("bne", "pal")

    # ---- draw both halves once, switching bank between them ----
    a.ins("lda #$00")
    a.ins("sta BANK_REG")                   # bank 0 is the one in the window
    a.ins("sta PPU_MASK")                   # picture off while it is filled
    a.ins("lda #$00")
    a.ins("sta ZP_LO")                      # and the copy starts at row 0
    a.ins("lda #${0:02X}".format(0x8000 >> 8))
    a.ins("sta ZP_HI")                      # bank 0's table is at $8000
    a.ins("jsr BANK_DRAW")
    a.ins("lda #1")
    a.ins("sta BANK_REG")                   # the write that switches banks
    a.ins("lda #${0:02X}".format(0x8000 >> 8))
    a.ins("sta ZP_HI")                      # bank 1's table is at $8000 too
    a.ins("jsr BANK_DRAW")

    a.ins("lda #$08")                       # background on
    a.ins("sta PPU_MASK")

    # ---- the game: one frame at a time, both banks each frame ----
    a.label("MAIN")
    a.label("wait")
    a.ins("bit PPU_STATUS")                 # bit 7 says a new frame has begun
    a.branch("bpl", "wait")                 # and reading the register clears it
    a.ins("inc VAR_FRAME")                  # the game's own clock
    a.ins("lda VAR_FRAME")
    a.ins("sta PPU_SCROLL")                 # and the picture slides right,
    a.ins("lda #$00")                       # one pixel per frame
    a.ins("sta PPU_SCROLL")

    # Both halves, one after the other, and the register changes in between.
    # This is the whole trick of the stage: forty kilobytes of program are
    # used in one frame, and never more than sixteen of them are visible at
    # any one moment.
    a.ins("lda #0")
    a.ins("sta BANK_REG")                   # bank 0 in the window
    a.ins("jsr BANK_DRAW")                  # its half of the picture
    a.ins("lda #1")
    a.ins("sta BANK_REG")                   # the store that switches banks
    a.ins("jsr BANK_DRAW")                  # the other half
    a.ins("lda #0")
    a.ins("sta BANK_REG")                   # bank 0 back in the window
    a.ins("jmp MAIN")

    code = a.resolve()
    if ORIGIN_FIXED + len(code) > CHR_AT:
        raise ValueError("the fixed bank's code runs into its tables")

    image = bytearray(BANK_SIZE)
    image[:len(code)] = code

    tiles_at = CHR_AT - ORIGIN_FIXED
    a.note(ORIGIN_FIXED, tiles_at, "RESET and the game loop")

    # the two tables, at the addresses their labels promised
    # Two tile shapes. Each pixel in a shape is one bit, so the bytes below
    # are eight pixels each: tile 0 is a solid square, tile 1 a checkerboard.
    tiles = bytes([0xFF] * 8 + [0x00] * 8 +
                  [0xAA] * 8 + [0x55] * 8)
    image[tiles_at:tiles_at + len(tiles)] = tiles
    a.note(CHR_AT, len(tiles),
           "CHR_TILES: two tile shapes, 16 bytes each")

    # The palette, in order: the colour the screen shows where nothing is
    # drawn, then four. A tile says "pixel value 1"; the palette says which
    # colour that is, so one tile can appear in two colours without a second
    # copy of the tile.
    # Sixteen colours written twice. The chip's palette byte is a colour
    # *number*, so the table has to be indexed by that number: the first
    # entries are the dark shades, the later ones the bright ones, and a
    # pixel value of 1 means entry 1, not the second entry of a short list.
    palette = bytes([0x0F, 0x01, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07,
                     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F] * 2)
    pal_at = PAL_AT - ORIGIN_FIXED
    image[pal_at:pal_at + len(palette)] = palette
    a.note(PAL_AT, len(palette),
           "PALETTE: 32 colours; entry 0 is the background, the rest are the picture")

    # The three vectors, low byte first, each at its own address: the chip
    # reads $FFFC for the address to start at, not the word before it.
    vectors = (ORIGIN_FIXED.to_bytes(2, "little")     # $FFFA NMI
               + ORIGIN_FIXED.to_bytes(2, "little")   # $FFFC the reset vector
               + ORIGIN_FIXED.to_bytes(2, "little"))  # $FFFE IRQ
    image[BANK_SIZE - 6:] = vectors
    a.note(ORIGIN_FIXED + BANK_SIZE - 6, 6,
           "the vectors: NMI, the reset vector at $FFFC, and IRQ, all $C000")

    assert ORIGIN_FIXED + len(code) <= PAL_AT, "the code runs into the palette"
    return bytes(image), a


HEADER = """/*
 * cartridge.h — the game, as bytes. Generated by host/asm.py; edit that
 * script, not this file.
 *
 * A cartridge is a printed circuit board with memory chips on it, and this
 * array is what is on those chips: bank 0 and bank 1 are the pieces the game
 * switches between, and the fixed bank is the piece that is always visible
 * at $C000. The banks are 16 KB each because that is the size of the
 * processor's window, not because the chips were that size.
 *
 * There is no header and no file name here: the cartridge is not a file any
 * more, it is memory the processor reads directly.
 */
#pragma once
#include <stdint.h>

/* one bank, in bytes: 16 KB, the size of the window it appears in */
#define CARTRIDGE_BANK 0x4000

/* how many banks the game can switch between: two of them, in this cartridge */
#define CARTRIDGE_BANKS 2
"""

TRAILER = """/* What the bank register can select, in one place: the bus uses this table
 * instead of deciding for itself where each bank lives. Adding a third bank
 * to the cartridge means adding one line here and nothing else. */
static const uint8_t *const bank_pointers[CARTRIDGE_BANKS] = {
    bank_0,
    bank_1,
};
"""


# ------------------------------------------------- printing it as C
def print_c(image, parts):
    """The same bytes as three C arrays, one region at a time.

    Each array is as long as a bank, but only the bytes the program actually
    wrote are listed; C fills the rest with zeros. That is not a way to save
    typing — it is the honest picture. A bank is 16 KB of address space, and
    this game uses a few dozen bytes of it. Real games filled theirs, which
    is exactly why one bank was not enough for them.
    """
    print(HEADER)
    for index, (name, a, code) in enumerate(parts):
        label = "0" if index == 0 else ("1" if index == 1 else "fixed")
        print("/* --- %s: the cartridge bank the test program loads as %s --- */" % (
            "the fixed bank, always at $C000" if label == "fixed"
            else "switchable bank " + label, label))
        print("static const uint8_t bank_%s[CARTRIDGE_BANK] = {" % label)
        for at, n, text in a.notes:
            print("    /* $%04X: %s */" % (a.org + at, text))
            if n == 0:
                continue                      # a marker, not a region
            chunk = bytes(code[at:at + n])
            while chunk and chunk[-1] == 0:
                chunk = chunk[:-1]
            # The address is written out as well as the bytes. Without it the
            # regions would end up next to each other instead of where the
            # program expects them, and every table in the cartridge would be
            # at the wrong address.
            for j in range(0, len(chunk), 12):
                print("        [0x%04X] =" % (at + j) if j == 0
                      else "        /* %04X */" % (at + j), end=" ")
                print(" ".join("0x%02X," % b for b in chunk[j:j + 12]))
        print("};")
        print()

    print(TRAILER)


def build():
    bank0, asm0 = build_bank(0)
    bank1, asm1 = build_bank(1)
    fixed, asmf = build_fixed()
    return (bank0 + bank1 + fixed,
            (("bank 0", asm0, bank0), ("bank 1", asm1, bank1), ("fixed", asmf, fixed)))


def main():
    image, parts = build()
    if len(sys.argv) > 1 and sys.argv[1] == "--listing":
        for name, a, code in parts:
            print("; ---- %s ----" % name)
            shown = sorted([(off, n, t, False) for off, n, t in a.listing]
                           + [(off, n, t, True) for off, n, t in a.notes])
            for off, n, text, is_note in shown:
                raw = code[off:off + n] if not is_note else b""
                print("%04X  %-23s %s" % (a.org + off,
                                          " ".join("%02X" % b for b in raw), text))
    elif len(sys.argv) > 1:
        with open(sys.argv[1], "wb") as f:
            f.write(image)
        print("wrote %s: %d bytes" % (sys.argv[1], len(image)))
    else:
        print_c(image, parts)


if __name__ == "__main__":
    main()
