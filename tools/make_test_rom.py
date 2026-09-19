#!/usr/bin/env python3
"""
make_test_rom.py — builds a legal NES ROM (iNES, NROM, mapper 0) that
exercises everything the emulator must get right: CHR pattern tables,
nametables, the attribute table, palettes, sprites (OAM DMA), NMI, VRAM
writes during vblank and hardware scrolling.

All the artwork (8x8 font and shapes) is generated here — the ROM is
100% our own work, so it can live in the repository.

    python3 tools/make_test_rom.py           -> build/test.nes
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm6502 import Assembler  # noqa: E402

# ----------------------------------------------------------------- font
FONT = {
    '0': [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    '1': ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    '2': [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    '3': ["####.", "....#", "....#", ".###.", "....#", "....#", "####."],
    '4': ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    '5': ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    '6': ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    '7': ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    '8': [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    '9': [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'B': ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    'C': [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    'D': ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    'E': ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    'F': ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    'G': [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    'H': ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'I': [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    'J': ["....#", "....#", "....#", "....#", "#...#", "#...#", ".###."],
    'K': ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    'L': ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    'O': [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'P': ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    'Q': [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    'R': ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    'U': ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'V': ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    'W': ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    'X': ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    'Y': ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    'Z': ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    ' ': [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    '-': [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    '.': [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    '!': ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    ':': [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
}

SHAPES = {
    # name -> 8 rows of 8 pixels (tile index assigned after the font)
    "brick": ["########", "#..#...#", "########", "#..#...#",
              "########", "#..#...#", "########", "........"],
    "block": ["########", "#......#", "#......#", "#......#",
              "#......#", "#......#", "#......#", "########"],
    "coin": ["..####..", ".#....#.", "#.#..#.#", "#......#",
             "#.#..#.#", "#..##..#", ".#....#.", "..####.."],
    "heart": [".##..##.", "########", "########", "########",
              ".######.", "..####..", "...##...", "........"],
    "check": ["#.#.#.#.", ".#.#.#.#", "#.#.#.#.", ".#.#.#.#",
              "#.#.#.#.", ".#.#.#.#", "#.#.#.#.", ".#.#.#.#"],
    "shade": ["########", ".######.", "..####..", "...##...",
              "...##...", "..####..", ".######.", "########"],
    "qmark": [".#####..", "##...##.", ".....##.", "...###..",
              "...##...", "........", "...##...", "........"],
    "grass": ["..####..", ".######.", "########", "########",
              "########", "########", "########", "########"],
}

FONT_FIRST = 1              # tile 0 stays empty (blank)
SHAPE_FIRST = None          # assigned in build_chr()


def glyph_tile(rows5):
    """5x7 glyph -> 8 bytes (8x8, one pixel of padding)."""
    out = []
    for r in range(8):
        row = rows5[r] if r < len(rows5) else "....."
        bits = 0
        for c, ch in enumerate(row):
            if ch == '#':
                bits |= 1 << (6 - c)
        out.append(bits)
    return out


def shape_tile(rows8):
    out = []
    for row in rows8:
        bits = 0
        for c, ch in enumerate(row):
            if ch == '#':
                bits |= 1 << (7 - c)
        out.append(bits)
    return out


def build_chr():
    """512 tiles (8 KB): font + shapes in both pattern tables."""
    tiles = []
    tiles.append([0] * 8)                       # tile 0: empty
    index = {}
    for ch, rows in FONT.items():
        index[ch] = len(tiles)
        tiles.append(glyph_tile(rows))
    shape_index = {}
    for name, rows in SHAPES.items():
        shape_index[name] = len(tiles)
        tiles.append(shape_tile(rows))
    while len(tiles) < 256:                     # pad to one pattern table
        tiles.append([0] * 8)

    # each tile is 16 bytes: 8 low plane + 8 high plane (we use plane 0 only)
    data = bytearray()
    for t in tiles:
        data += bytes(t)
        data += bytes(8)                        # high bitplane stays 0
    table = bytes(data)
    return table + table, index, shape_index   # both pattern tables


def build_nametable(index, shape):
    """30 rows x 32 columns of tile indices + the attribute table."""
    blank = index[' ']
    nt = [[blank] * 32 for _ in range(30)]

    def text(row, col, s):
        for i, ch in enumerate(s):
            if col + i < 32:
                nt[row][col + i] = index[ch]

    text(1, 2, "MINI MARIO NES TEST")
    text(3, 2, "FRAME:")
    text(5, 1, "SHAPES")
    for i, name in enumerate(["brick", "coin", "heart", "check", "shade",
                              "qmark", "grass", "block"]):
        nt[7][2 + i * 3] = shape[name]
    text(10, 1, "PALETTES")
    for p in range(4):
        for c in range(6):
            nt[12][2 + p * 7 + c] = shape["block"]
    text(15, 2, "SCROLLING WORLD 0123456789")
    text(17, 2, "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    text(19, 2, "THE QUICK BROWN FOX JUMPS")
    text(21, 2, "OVER THE LAZY DOG. 1234567890")
    text(24, 2, "WWW.PRZBAT.PL - HOMEBREW ROM")
    text(26, 2, "EMULATOR BRING-UP TEST ROM")
    text(28, 2, "COMMODORE 64 WAS SLOWER!")

    # attribute table: give each 16x16 block around the shapes/rows some colour
    attr = [0] * 64
    for by in range(8):
        for bx in range(8):
            v = 0
            if by in (3, 4):                     # the shape row: palette 1
                v = 0x55
            elif by == 5:                        # palettes row
                v = (bx & 1) * 0x55
            elif by in (7, 8):                   # text block: palette 2
                v = 0xAA
            elif by >= 12:
                v = 0x55
            attr[by * 8 + bx] = v

    flat = []
    for row in nt:
        flat += row
    return flat, attr


def build_program(index, shape, nt_bytes, attr_bytes):
    # NROM-128: a 16 KB bank mirrored at both $8000 and $C000, so we
    # assemble at $C000 and the vectors land at the end of the bank
    a = Assembler(0xC000)
    L = a.line          # accepts several lines at once
    # header: reset + NMI + IRQ vectors are appended at the end
    L("reset:")
    L("SEI", "CLD", "LDX #$FF", "TXS")   # stack at $01FF
    L("LDA #$00", "STA $2000", "STA $2001")     # rendering off while we set up
    L("wait1:", "LDA $2002", "BPL wait1")
    L("wait2:", "LDA $2002", "BPL wait2")

    # clear OAM (all sprites at y = $FF -> off screen)
    L("LDA #$FF", "LDX #$00")
    L("oamclr:", "STA $0200,X", "INX", "BNE oamclr")

    # palette
    L("LDA #$3F", "STA $2006", "LDA #$00", "STA $2006")
    L("LDX #$00")
    L("palloop:", "LDA pals,X", "STA $2007", "INX", "CPX #$20", "BNE palloop")

    # nametable + attribute table (generated from the Python layout)
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    for b in nt_bytes:
        L(f"LDA #${b:02X}", "STA $2007")
    for b in attr_bytes:
        L(f"LDA #${b:02X}", "STA $2007")

    # sprites in the shadow OAM at $0200 (four sprites)
    sprites = [
        (70, shape["coin"], 0x00, 40),    # y, tile, attr, x
        (70, shape["heart"], 0x01, 60),
        (70, shape["qmark"], 0x42, 180),  # flipped horizontally
        (150, shape["shade"], 0x81, 120),  # flipped vertically
    ]
    for i, (sy, tile, attr, sx) in enumerate(sprites):
        L(f"LDA #${sy:02X}", f"STA ${0x0200 + i * 4:04X}")
        L(f"LDA #${tile:02X}", f"STA ${0x0200 + i * 4 + 1:04X}")
        L(f"LDA #${attr:02X}", f"STA ${0x0200 + i * 4 + 2:04X}")
        L(f"LDA #${sx:02X}", f"STA ${0x0200 + i * 4 + 3:04X}")

    # scroll reset + rendering on + NMI on
    L("LDA #$00", "STA $2005", "STA $2005")
    L("LDA #$1E", "STA $2001")     # BG + sprites, left columns shown
    L("LDA #$80", "STA $2000")     # NMI on

    # main loop: once per frame, move sprite 2 and push OAM with DMA.
    # (the frame counter lives in zero page: $10 = counter, $11 = frames)
    L("main:")
    L("waitvb:", "LDA $2002", "BPL waitvb")
    L("INC $10")
    L("LDA $10", "LSR A", "LSR A", "CLC", "ADC #60")
    L("STA $020B")                 # sprite 2 x position (shadow OAM)
    L("LDA #$02", "STA $4014")     # OAM DMA from $0200
    L("JMP main")

    # NMI: count frames, scroll, and print the counter into the nametable
    L("nmi:")
    L("PHA", "TXA", "PHA", "TYA", "PHA")
    L("INC $11")                                           # frame counter
    # VRAM updates first: $2006 clobbers the scroll register, so the
    # scroll must be written LAST for the pre-render latch to pick it up
    L("LDA #$20", "STA $2006", "LDA #$43", "STA $2006")    # row 3, col 3
    L("LDA $11", "AND #$0F", "TAX", "LDA digits,X", "STA $2007")
    L("LDA #$20", "STA $2006", "LDA #$44", "STA $2006")
    L("LDA $11", "LSR A", "LSR A", "LSR A", "LSR A", "TAX")
    L("LDA digits,X", "STA $2007")
    L("LDA $11", "STA $2005", "LDA #$00", "STA $2005")     # scroll x = frame
    L("PLA", "TAY", "PLA", "TAX", "PLA", "RTI")

    L("irq:")
    L("RTI")

    L("pals:")
    L(".byte $0F,$21,$11,$30")     # backdrop, blue ramp
    L(".byte $0F,$16,$27,$30")     # red / orange
    L(".byte $0F,$1A,$2A,$30")     # green
    L(".byte $0F,$12,$22,$30")     # dark blue
    L(".byte $0F,$16,$27,$30")     # sprite 0
    L(".byte $0F,$1A,$2A,$30")     # sprite 1
    L(".byte $0F,$12,$22,$30")     # sprite 2
    L(".byte $0F,$30,$10,$00")     # sprite 3

    L("digits:")
    L(".byte " + ",".join(f"${index[str(d)]:02X}" for d in range(10)))

    # vectors
    L(".org $FFFA")
    L(".word nmi, reset, irq")

    return a.assemble()


# ----------------------------------------------------------------- MMC1
# A second cartridge that exercises the MMC1: PRG banking, CHR banking
# and the nametable mirroring modes. The results are stored in RAM at
# $0300.. and also shown on screen as digits (1 = pass, 0 = fail).

def mmc1_write(a, value, addr, label=""):
    """Emit the five serial writes that load one MMC1 register."""
    for i in range(5):
        a.line(f"; MMC1 {label} bit {i}")
        a.line(f"LDA #${(value >> i) & 1:02X}", f"STA ${addr:04X}")


def build_mmc1_rom(index, shape):
    """A cartridge that exercises the MMC1: PRG banking, CHR banking and
    the nametable mirroring modes. Results land in RAM at $0300..$030F and
    are also shown on screen as digits (1 = pass)."""
    from asm6502 import Assembler
    BANKS = 4
    main_bank = BANKS - 1

    # ---- switchable banks: a routine at $8000 leaving a marker in RAM
    banks, magics = [], []
    for n in range(main_bank):
        magic = 0xA5 ^ (n * 0x11)
        magics.append(magic)
        b = Assembler(0x8000)
        b.line(f"bank{n}_entry:")
        b.line(f"LDA #${magic:02X}")
        b.line(f"STA ${0x0300 + n:04X}")
        b.line("RTS")
        banks.append(b.assemble()[:16384].ljust(16384, b"\xFF"))

    # ---- the fixed bank ($C000, PRG mode 3): tests then display
    a = Assembler(0xC000)
    L = a.line

    def mmc1(value, addr, label):
        for i in range(5):
            L(f"; MMC1 {label}: bit {i}")
            L(f"LDA #${(value >> i) & 1:02X}", f"STA ${addr:04X}")

    L("reset:")
    L("SEI", "CLD", "LDX #$FF", "TXS")
    L("LDA #$00", "STA $2000", "STA $2001")     # rendering off
    L("w1:", "LDA $2002", "BPL w1")
    L("w2:", "LDA $2002", "BPL w2")
    L("LDA #$FF", "LDX #$00")
    L("ocl:", "STA $0200,X", "INX", "BNE ocl")

    # ================= test 1: PRG banking =================
    mmc1(0x0C, 0x8000, "control: PRG mode 3, CHR 8K, one-screen low")
    for n in range(main_bank):
        mmc1(n, 0xE000, f"PRG bank {n}")
        L("JSR $8000")                          # the bank's own routine
    L("LDA #$00", "STA $08")
    for n, magic in enumerate(magics):
        L(f"LDA ${0x0300 + n:04X}")
        L(f"CMP #${magic:02X}")
        L(f"BNE skip{n}")
        L("LDA $08", "ORA #$01", "STA $08")
        L(f"skip{n}:")

    # ================= test 2: CHR banking =================
    mmc1(0x02, 0xA000, "CHR bank 1 (8K units)")
    L("LDA #$00", "STA $2006", "STA $2006")     # PPUADDR = $0000
    L("LDA $2007")                              # discard the buffered read
    L("LDA $2007")
    L("CMP #$FF")                               # marker byte of CHR bank 1
    L("BNE skipc")
    L("LDA $08", "ORA #$02", "STA $08")
    L("skipc:")
    mmc1(0x00, 0xA000, "CHR bank 0 again")
    L("LDA #$00", "STA $2006", "STA $2006")

    # ================= test 3: mirroring =================
    mmc1(0x0C, 0x8000, "control: one-screen lower")
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA #$5A", "STA $2007")                  # $2000 = $5A
    L("LDA #$2C", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA $2007", "LDA $2007")                 # read $2C00
    L("CMP #$5A")                               # one-screen: aliases
    L("BNE skipm")
    L("LDA $08", "ORA #$04", "STA $08")
    L("skipm:")
    mmc1(0x0F, 0x8000, "control: horizontal")
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA #$A5", "STA $2007")                  # $2000 = $A5
    L("LDA #$28", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA $2007", "LDA $2007")                 # $2800: other nametable
    L("CMP #$A5")
    L("BEQ skipm2")                             # must NOT match
    L("LDA $08", "ORA #$08", "STA $08")
    L("skipm2:")
    L("LDA $08", "STA $030F")                   # result byte for the host

    # ================= display =================
    L("LDA #$3F", "STA $2006", "LDA #$00", "STA $2006")
    L("LDX #$00")
    L("pl:", "LDA pals,X", "STA $2007", "INX", "CPX #$20", "BNE pl")

    lines = {}
    def put(row, col, s):
        for i, ch in enumerate(s):
            lines.setdefault(row, {})[col + i] = index[ch]
    put(2, 2, "MMC1 CARTRIDGE TEST")
    put(4, 1, "1 PRG BANKING")
    put(6, 1, "2 CHR BANKING")
    put(8, 1, "3 MIRRORING")
    put(11, 1, "RESULTS 1 OK 0 FAIL")
    put(14, 1, "PRG MODE 3 FIXED LAST")
    put(16, 1, "SERIAL REGISTER WRITES")
    put(18, 1, "ONE SCREEN AND HORIZONTAL")
    put(20, 1, "CHR 8K BANK SWITCHING")
    put(22, 1, "ALL ORIGINAL HOMEBREW ROM")

    result_cells = {1: (4, 22), 2: (6, 22), 3: (8, 22)}
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    for r in range(30):
        cells = [index[' ']] * 32
        for c, t in lines.get(r, {}).items():
            cells[c] = t
        for t, (rr, cc) in result_cells.items():
            if rr == r:
                cells[cc] = index['0']
        for b in cells:
            L(f"LDA #${b:02X}", "STA $2007")
    for _ in range(64):
        L("LDA #$00", "STA $2007")              # attribute table

    for t, (r, c) in result_cells.items():
        L("LDA $08", f"AND #${1 << (t - 1):02X}")
        L(f"BEQ rf{t}")
        L(f"LDA #${index['1']:02X}")
        L(f"JMP rd{t}")
        L(f"rf{t}:", f"LDA #${index['0']:02X}")
        L(f"rd{t}:", "PHA")
        # nametable address = $2000 + row*32 + column; getting this wrong
        # puts the digit in the other nametable (or another row) and the
        # screen quietly shows the placeholder zero instead
        off = r * 32 + c
        L(f"LDA #${0x20 + (off >> 8):02X}", "STA $2006")
        L(f"LDA #${off & 0xFF:02X}", "STA $2006")
        L("PLA", "STA $2007")

    # the scroll register t also carries the nametable bits, and the VRAM
    # address writes above clobbered them - so PPUCTRL must be written
    # again before the scroll, exactly as a real game does
    L("LDA #$00", "STA $2000")                  # nametable 0, pattern 0
    L("LDA #$00", "STA $2005", "STA $2005")
    L("LDA #$0A", "STA $2001")                  # background on
    L("main:", "JMP main")
    L("nmi:", "RTI")
    L("irq:", "RTI")
    L("pals:")
    L(".byte $0F,$21,$11,$30")
    L(".byte $0F,$16,$27,$30")
    L(".byte $0F,$1A,$2A,$30")
    L(".byte $0F,$12,$22,$30")
    L(".byte $0F,$16,$27,$30")
    L(".byte $0F,$1A,$2A,$30")
    L(".byte $0F,$12,$22,$30")
    L(".byte $0F,$30,$10,$00")
    L(".org $FFFA")
    L(".word nmi, reset, irq")
    main_code = a.assemble()

    chr_font = build_chr()[0][:8192]            # bank 0: normal artwork
    chr_marker = bytes([0xFF] * 8192)           # bank 1: marker pattern

    header = bytearray(b"NES\x1a")
    header.append(BANKS)
    header.append(2)
    header.append(0x10)                         # mapper 1 (MMC1)
    header.append(0x00)
    header += bytes(8)

    prg = b"".join(banks) + main_code.ljust(16384, b"\xFF")
    return bytes(header) + prg + chr_font + chr_marker


def build_mmc3_rom(index, shape):
    """A cartridge that exercises the MMC3: 8 KB PRG banking through R6,
    1 KB and 2 KB CHR banking, the mirroring register and — the part no
    commercial cartridge here happens to use — the scanline counter's IRQ.
    Results land in RAM at $0300.. and are shown on screen (1 = pass).

    PRG layout: 64 KB = eight 8 KB banks. Banks 0-5 hold a tiny routine
    with a magic marker each, and the last two banks (which MMC3 always
    maps at $C000/$E000) hold this code."""
    from asm6502 import Assembler
    SWITCHABLE = 6                          # 8 KB banks 0..5 are testable

    banks8, magics = [], []
    for n in range(SWITCHABLE):
        magic = 0x5A ^ (n * 0x13)
        magics.append(magic)
        b = Assembler(0x8000)
        b.line(f"bank8_{n}:")
        b.line(f"LDA #${magic:02X}")
        b.line(f"STA ${0x0300 + n:04X}")
        b.line("RTS")
        banks8.append(b.assemble()[:0x2000].ljust(0x2000, b"\xFF"))

    a = Assembler(0xC000)                   # the fixed banks
    L = a.line

    def sel(reg, value, label=""):
        """MMC3 register write: bank select, then the data."""
        L(f"; MMC3 R{reg} = ${value:02X} {label}")
        L(f"LDA #${reg:02X}", "STA $8000")
        L(f"LDA #${value:02X}", "STA $8001")

    def reg(addr, value, label=""):
        L(f"; MMC3 ${addr:04X} = ${value:02X} {label}")
        L(f"LDA #${value:02X}", f"STA ${addr:04X}")

    L("reset:")
    L("SEI", "CLD", "LDX #$FF", "TXS")
    L("LDA #$00", "STA $2000", "STA $2001")     # rendering off
    L("w1:", "LDA $2002", "BPL w1")
    L("w2:", "LDA $2002", "BPL w2")
    L("LDA #$FF", "LDX #$00")
    L("ocl:", "STA $0200,X", "INX", "BNE ocl")
    L("LDA #$00", "STA $0300", "STA $0310", "STA $0311")

    # start state: display font in CHR, vertical mirroring, IRQ off
    L("LDA #$00", "STA $E000")                  # IRQ off + acknowledge
    sel(0, 0x00, "2 KB CHR at $0000")
    sel(1, 0x02, "2 KB CHR at $0800")
    sel(2, 0x04, "1 KB CHR at $1000")
    sel(3, 0x05, "1 KB CHR at $1400")
    sel(4, 0x06, "1 KB CHR at $1800")
    sel(5, 0x07, "1 KB CHR at $1C00")
    reg(0xA000, 0x00, "vertical mirroring")

    # ================= test 1: 8 KB PRG banking =================
    for n in range(SWITCHABLE):
        sel(6, n, f"PRG bank {n} at $8000")
        L("JSR $8000")
    L("LDA #$00", "STA $08")
    for n, magic in enumerate(magics):
        L(f"LDA ${0x0300 + n:04X}")
        L(f"CMP #${magic:02X}")
        L(f"BNE pskip{n}")
        L("LDA $08", "ORA #$01", "STA $08")
        L(f"pskip{n}:")

    # ============ test 2: 1 KB and 2 KB CHR banking ============
    # CHR is two 8 KB halves: banks 0-7 the font, banks 8-15 a marker.
    sel(2, 0x08, "1 KB CHR bank 8 (marker)")
    L("LDA #$10", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA $2007")                              # buffered read, discard
    L("LDA $2007")
    L("CMP #$FF")
    L("BNE cskip")
    L("LDA $08", "ORA #$02", "STA $08")
    L("cskip:")
    sel(2, 0x04, "font again")

    sel(0, 0x08, "2 KB CHR bank 8 (marker)")
    L("LDA #$00", "STA $2006", "STA $2006")
    L("LDA $2007")
    L("LDA $2007")
    L("CMP #$FF")
    L("BNE c2skip")
    L("LDA $08", "ORA #$04", "STA $08")
    L("c2skip:")
    sel(0, 0x00, "font again")

    # ================= test 3: mirroring =================
    reg(0xA000, 0x00, "vertical")
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA #$5A", "STA $2007")                  # $2000 = $5A
    L("LDA #$28", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA $2007", "LDA $2007")                 # read $2800: aliases $2000
    L("CMP #$5A")
    L("BNE mskip")
    L("LDA $08", "ORA #$08", "STA $08")
    L("mskip:")
    reg(0xA000, 0x01, "horizontal")
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA #$A5", "STA $2007")                  # $2000 = $A5
    L("LDA #$28", "STA $2006", "LDA #$00", "STA $2006")
    L("LDA $2007", "LDA $2007")                 # $2800 is now a different table
    L("CMP #$A5")
    L("BEQ m2skip")
    L("LDA $08", "ORA #$10", "STA $08")
    L("m2skip:")
    reg(0xA000, 0x00, "vertical again")

    # ================= test 4: the scanline counter IRQ =================
    # Latch 100: one interrupt per frame, at line 100. The handlers count
    # frames (NMI) and interrupts (IRQ), so the result does not depend on
    # how fast the emulator is.
    reg(0xC000, 240, "latch: one interrupt per frame")
    reg(0xC001, 0, "reload")
    reg(0xE001, 0, "IRQ on")
    L("LDA #$80", "STA $2000")                  # NMI on: the frame counter
    L("CLI")                                    # interrupts enabled, or none arrive
    L("LDA #$08", "STA $2001")                  # drawing on: the counter only
    L("irqwait:", "LDA $0311", "CMP #$04", "BCC irqwait")   # ticks while the
    L("LDA #$00", "STA $2001")                  # PPU fetches patterns
    reg(0xE000, 0, "IRQ off + acknowledge")
    L("LDA $0310")
    L("CMP #$03")                               # at least 3 of 4 frames
    L("BCC iskip")
    L("CMP #$07")                               # and not more than 6
    L("BCS iskip")
    L("LDA $08", "ORA #$20", "STA $08")
    L("iskip:")
    L("LDA $08", "STA $030F")                   # result byte for the host

    # ================= display =================
    L("LDA #$3F", "STA $2006", "LDA #$00", "STA $2006")
    L("LDX #$00")
    L("pl:", "LDA pals,X", "STA $2007", "INX", "CPX #$20", "BNE pl")

    lines = {}
    def put(row, col, s):
        for i, ch in enumerate(s):
            lines.setdefault(row, {})[col + i] = index[ch]
    put(2, 2, "MMC3 CARTRIDGE TEST")
    put(4, 1, "1 PRG 8K BANKING")
    put(6, 1, "2 CHR 1K AND 2K BANKS")
    put(8, 1, "3 MIRRORING REGISTER")
    put(10, 1, "4 SCANLINE IRQ")
    put(13, 1, "RESULTS 1 OK 0 FAIL")
    put(16, 1, "R6 PICKS 8000")
    put(18, 1, "LAST TWO BANKS FIXED")
    put(20, 1, "LATCH 240 ONE IRQ A FRAME")
    put(22, 1, "ALL ORIGINAL HOMEBREW ROM")

    result_cells = {1: (4, 24), 2: (6, 24), 3: (8, 24), 4: (10, 24)}
    L("LDA #$20", "STA $2006", "LDA #$00", "STA $2006")
    for r in range(30):
        cells = [index[' ']] * 32
        for c, t in lines.get(r, {}).items():
            cells[c] = t
        for t, (rr, cc) in result_cells.items():
            if rr == r:
                cells[cc] = index['0']
        for b in cells:
            L(f"LDA #${b:02X}", "STA $2007")
    for _ in range(64):
        L("LDA #$00", "STA $2007")              # attribute table

    for t, (r, c) in result_cells.items():
        L("LDA $08", f"AND #${1 << (t - 1):02X}")
        L(f"BEQ rf{t}")
        L(f"LDA #${index['1']:02X}")
        L(f"JMP rd{t}")
        L(f"rf{t}:", f"LDA #${index['0']:02X}")
        L(f"rd{t}:", "PHA")
        # nametable address = $2000 + row*32 + column; getting this wrong
        # puts the digit in the other nametable (or another row) and the
        # screen quietly shows the placeholder zero instead
        off = r * 32 + c
        L(f"LDA #${0x20 + (off >> 8):02X}", "STA $2006")
        L(f"LDA #${off & 0xFF:02X}", "STA $2006")
        L("PLA", "STA $2007")

    L("LDA #$00", "STA $2000")                  # nametable 0, pattern 0
    L("LDA #$00", "STA $2005", "STA $2005")
    L("LDA #$0A", "STA $2001")                  # background on
    L("main:", "JMP main")

    L("nmi:")
    L("PHA")
    L("INC $0311")
    L("PLA")
    L("RTI")

    L("irq:")
    L("PHA")
    L("INC $0310")
    L("LDA #$00", "STA $E000")                  # acknowledge (disables too)
    L("LDA #$00", "STA $C001")                  # reload for the next frame
    L("LDA #$00", "STA $E001")                  # and arm it again
    L("PLA")
    L("RTI")

    L("pals:")
    L(".byte $0F,$21,$11,$30")
    L(".byte $0F,$16,$27,$30")
    L(".byte $0F,$1A,$2A,$30")
    L(".byte $0F,$12,$22,$30")
    L(".byte $0F,$16,$27,$30")
    L(".byte $0F,$1A,$2A,$30")
    L(".byte $0F,$12,$22,$30")
    L(".byte $0F,$30,$10,$00")
    L(".org $FFFA")
    L(".word nmi, reset, irq")
    main_code = a.assemble()

    chr_all = build_chr()[0][:8192]
    chr_font = chr_all[:8192].ljust(8192, b"\x00")      # 1 KB banks 0-7
    chr_marker = bytes([0xFF] * 8192)                   # 1 KB banks 8-15

    header = bytearray(b"NES\x1a")
    header.append(4)                            # 4 x 16 KB = 64 KB PRG
    header.append(2)                            # 2 x 8 KB CHR
    header.append(0x41)                         # mapper 4, vertical mirroring
    header.append(0x00)
    header += bytes(8)

    prg = b"".join(banks8) + main_code.ljust(16384, b"\xFF")
    return bytes(header) + prg + chr_font + chr_marker


def main():
    chr_data, index, shape = build_chr()
    nt_bytes, attr_bytes = build_nametable(index, shape)
    prog = build_program(index, shape, nt_bytes, attr_bytes)

    # pad PRG to 16 KB
    PRG_SIZE = 16384
    if len(prog) > PRG_SIZE:
        print(f"error: program is {len(prog)} bytes, PRG bank is {PRG_SIZE}")
        return 1
    prg = prog + bytes([0xFF] * (PRG_SIZE - len(prog)))

    header = bytearray(b"NES\x1a")
    header.append(1)             # 1 x 16 KB PRG
    header.append(1)             # 1 x 8 KB CHR
    header.append(0x01)          # flags6: vertical mirroring (scrolls sideways)
    header.append(0x00)          # flags7: NROM
    header += bytes(8)           # padding

    rom = bytes(header) + prg + chr_data
    os.makedirs("build", exist_ok=True)
    if "--mmc1" in sys.argv:
        mmc1 = build_mmc1_rom(index, shape)
        path = os.path.join("build", "mmc1.nes")
        with open(path, "wb") as f:
            f.write(mmc1)
        print(f"wrote {path}: {len(mmc1)} bytes (MMC1, 4 x 16K PRG, 2 x 8K CHR)")
        return 0
    if "--mmc3" in sys.argv:
        mmc3 = build_mmc3_rom(index, shape)
        path = os.path.join("build", "mmc3.nes")
        with open(path, "wb") as f:
            f.write(mmc3)
        print(f"wrote {path}: {len(mmc3)} bytes (MMC3, 4 x 16K PRG, 2 x 8K CHR)")
        return 0
    path = os.path.join("build", "test.nes")
    with open(path, "wb") as f:
        f.write(rom)
    print(f"wrote {path}: {len(rom)} bytes "
          f"(PRG {len(prog)} used / {PRG_SIZE}, CHR {len(chr_data)})")
    print(f"  font tiles: 0-{len(FONT)}  shapes: "
          + ", ".join(f"{n}={t}" for n, t in shape.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
