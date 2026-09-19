/*
 * Stage 15 — why it runs slowly, and what to do about it.
 *
 * Stage 14's game works, and it is slow: the picture arrives in jerks and the
 * processor spends its life waiting. The temptation is to guess what is
 * expensive and start rewriting it. This stage does the opposite — it measures
 * first, and only then changes what the measurement points at.
 *
 * Three things make that possible, and all three are in this file:
 *
 *   cycles     the emulated processor counts the ticks it has used, as the real
 *              chip did. That is the only way to know how much of a frame has
 *              gone by: the number of instructions per frame is not fixed, the
 *              number of ticks is (about 29,780 of them, 60 times a second).
 *   a clock    clock() from the C library counts the time this program has
 *              spent on the computer's processor. Two calls per phase turn a
 *              frame into the table at the end of the run.
 *   four runs  the same frames are emulated four times: as they were, then with
 *              one change, then with two, then with three. The table says what
 *              each change bought, and a checksum says the picture never moved.
 *
 * The game is the small cartridge further down: a landscape the program draws
 * once, and a bird that flies across the sky. Its picture is almost still,
 * which is exactly the case the last and largest change is about.
 *
 * Everything here runs on the computer, not on the board: a table of times is
 * easier to read in a terminal than on a panel, and this stage is about the
 * emulator's own work. What the panel costs to talk to — the wire of stage 04 —
 * is not in these numbers and is not what is being fixed here.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------ the picture
 * A screen of the console is 256 by 240 pixels, built from tiles of 8 by 8:
 * 32 tiles across, 30 rows down. Nothing here has changed since stage 08.
 */
#define PPU_W     256
#define PPU_H     240
#define TILE      8
#define TILE_COLS (PPU_W / TILE)          /* 32 tiles across      */
#define TILE_ROWS (PPU_H / TILE)          /* 30 bands down        */
#define WORLD_W   (TILE_COLS * TILE)      /* 256 pixels across    */
#define WORLD_H   (TILE_ROWS * TILE)      /* 240 pixels down      */

/* Eight tile shapes, sixteen bytes each: eight rows of two bitplanes. */
#define CHR_TILES 8
#define CHR_BYTES (CHR_TILES * 16)

/* One frame of the console is this many ticks of its processor. The chip runs
 * at about 1.79 MHz and the picture is made 60 times a second, so 1,790,000
 * divided by 60 is a little under 30,000. Running the game for exactly this
 * long is what keeps the emulated machine in step with the real one; the last
 * instruction of a frame may step over the line by a few ticks.
 */
#define CYCLES_PER_FRAME 29780u

/* How many frames each run emulates: three seconds of the console's time, and
 * enough for the times to settle down. */
#define FRAMES 180

#define TICKS_PER_INSTRUCTION 2u          /* every instruction costs at least this */

/* --------------------------------------------------------- the panel's colours
 * The same eight colours as stages 01 to 13, in the order the framebuffer
 * stores them: a byte in the picture is a number, and this table is what that
 * number means on the glass.
 */
#define COLOUR_COUNT 8
static const uint16_t panel_colours[COLOUR_COUNT] = {
    0x0000,   /* 0 black   */
    0xF800,   /* 1 red     */
    0x07E0,   /* 2 green   */
    0x001F,   /* 3 blue    */
    0xFFE0,   /* 4 yellow  */
    0xF81F,   /* 5 magenta */
    0x07FF,   /* 6 cyan    */
    0xFFFF,   /* 7 white   */
};

/* One letter per colour, so the picture can be looked at in a terminal. */
static const char colour_glyph[COLOUR_COUNT] = {
    '.', 'r', 'g', 'b', 'y', 'm', 'c', 'w',
};

/* The same eight colours with their two bytes already in the order the panel
 * wants them, most significant first. The idea is that the conversion per pixel
 * — two shifts and an or — turns into one look-up. Whether that is worth
 * anything is not a matter of opinion: the table at the end of the run answers
 * it, and the answer in this stage is "almost nothing".
 */
static uint16_t panel_words[COLOUR_COUNT];

static void build_panel_words(void)
{
    for (int i = 0; i < COLOUR_COUNT; i++) {
        uint16_t colour = panel_colours[i];
        panel_words[i] = (uint16_t)((colour >> 8) | (colour << 8));
    }
}

/* ------------------------------------------------------------- the cartridge
 * The game, as bytes. There is no cartridge file and no assembler here: the
 * program below is written out by hand, the way stage 13 wrote its demo out,
 * and the comments say what each piece does.
 *
 * The program starts at $C000, which the processor reaches through the reset
 * vector at the very top of the address space. In order it:
 *
 *   - writes four palette bytes to $3F00: blue, white, yellow, green. From
 *     then on a pixel of the picture is one of those four colours.
 *   - copies eight tile shapes (16 bytes each: two bitplanes, as in stage 08)
 *     to $0000, so the picture chip knows what a cloud, a hill or a bird
 *     looks like.
 *   - fills the picture, one tile per screen row, from a 30-byte table. That
 *     is the landscape: sky, clouds, hills, water, grass, a wall of bricks.
 *   - plants four trees at chosen cells.
 *   - switches the background on and puts the bird in the top left cell.
 *
 * Then it loops for ever, and the loop is the interesting part:
 *
 *   wait:  bit $2002 / bpl wait     spin until the picture chip says a new
 *                                   frame has begun
 *          count the frame, and on every fourth one erase the bird, move its
 *          cell one to the right, draw it again
 *
 * The bird walks the first eight rows of the picture, which are all sky, so the
 * cell it leaves behind is the cell it found. When it reaches the end of those
 * rows it starts again at the top left.
 */
static const uint8_t cart_code[] = {
    0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00,
    0x8D, 0x06, 0x20, 0xA2, 0x00, 0xBD, 0xD1, 0xC0, 0x8D, 0x07, 0x20, 0xE8,
    0xE0, 0x04, 0xD0, 0xF5, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20,
    0xA2, 0x00, 0xBD, 0xD5, 0xC0, 0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x80, 0xD0,
    0xF5, 0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA0,
    0x00, 0xB9, 0x55, 0xC1, 0xA2, 0x20, 0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xFA,
    0xC8, 0xC0, 0x1E, 0xD0, 0xF0, 0xA2, 0x00, 0xBD, 0x73, 0xC1, 0x8D, 0x06,
    0x20, 0xE8, 0xBD, 0x73, 0xC1, 0x8D, 0x06, 0x20, 0xE8, 0xBD, 0x73, 0xC1,
    0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x0C, 0xD0, 0xE7, 0xA9, 0x20, 0x8D, 0x06,
    0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x07, 0x8D, 0x07, 0x20, 0xA9,
    0x08, 0x8D, 0x01, 0x20, 0xA9, 0x00, 0x85, 0x00, 0xA9, 0x20, 0x85, 0x01,
    0xA9, 0x00, 0x85, 0x10, 0x2C, 0x02, 0x20, 0x2C, 0x02, 0x20, 0x2C, 0x02,
    0x20, 0x10, 0xFB, 0xE6, 0x10, 0xA5, 0x10, 0x29, 0x03, 0xD0, 0xED, 0xA5,
    0x01, 0x8D, 0x06, 0x20, 0xA5, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D,
    0x07, 0x20, 0x18, 0xA5, 0x00, 0x69, 0x01, 0x85, 0x00, 0xA5, 0x01, 0x69,
    0x00, 0x85, 0x01, 0xC9, 0x21, 0xD0, 0x04, 0xA9, 0x20, 0x85, 0x01, 0xA5,
    0x01, 0x8D, 0x06, 0x20, 0xA5, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x07, 0x8D,
    0x07, 0x20, 0x4C, 0x88, 0xC0,
    /* $C0D1: four colour numbers, the palette of the whole picture */
    0x03, 0x07, 0x04, 0x02,
    /* $C0D5: eight tile shapes, sixteen bytes each: two bitplanes per row */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x7E, 0x7E, 0x3C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3C,
    0x7E, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x55, 0xAA, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x81, 0xFF,
    0x81, 0x81, 0xFF, 0x81, 0x00, 0x7E, 0x7E, 0x00, 0x7E, 0x7E, 0x00, 0x7E,
    0xFF, 0xE7, 0xC3, 0xC3, 0xE7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xF7, 0xF7, 0xF7, 0x00, 0x00, 0x00, 0x30, 0x78, 0xFC, 0x78, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    /* $C155: one tile number per screen row, thirty rows */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02,
    0x03, 0x03, 0x04, 0x04, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    /* $C173: four trees: address high, address low, tile number */
    0x22, 0x44, 0x06, 0x22, 0x73, 0x06, 0x22, 0x8C, 0x06, 0x22, 0xD9, 0x06,
};

/* The reset vector: the two bytes at $FFFC say where the processor starts, and
 * $FFFA and $FFFE hold the same address for the two interrupt vectors, which
 * this game never uses. */
static const uint8_t cart_vectors[6] = {
    0x00, 0xC0,   /* $FFFA: NMI   */
    0x00, 0xC0,   /* $FFFC: reset */
    0x00, 0xC0,   /* $FFFE: IRQ   */
};

/* The cartridge is 32 KB of memory the processor can read at $8000 and above.
 * Loading it is one copy per piece: this is what stage 07 did by reading a file
 * from the computer's disk, with the file left out. Most of it stays zero. */
static uint8_t cart_rom[0x8000];

static void load_cartridge(void)
{
    memcpy(&cart_rom[0x4000], cart_code, sizeof cart_code);      /* at $C000 */
    memcpy(&cart_rom[0x7FFA], cart_vectors, sizeof cart_vectors); /* at $FFFA */
}

/* ------------------------------------------------------------------- the bus
 * The processor asks for an address, and this decides who answers: its own
 * memory, the picture chip, or the cartridge. Stage 06 built this map.
 */
static uint8_t work_ram[0x800];

uint8_t bus_read(uint16_t addr);
void    bus_write(uint16_t addr, uint8_t value);

/* -------------------------------------------------------------- the processor
 * The 6502 of stages 05, 06 and 13, with one addition: it keeps count of the
 * ticks it has used. Everything else is what it was.
 */
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_U 0x20
#define FLAG_V 0x40
#define FLAG_N 0x80

typedef struct {
    uint8_t  a;
    uint8_t  x, y;
    uint8_t  sp;
    uint8_t  status;
    uint16_t pc;
    uint32_t cycles;          /* ticks of the console's clock, this frame */
} cpu_t;

static cpu_t cpu;

/* Where the game stops when it meets an instruction this core does not have.
 * Printing which one beats running nonsense quietly. */
static uint16_t stopped_at;
static uint8_t  stopped_opcode;

static void set_flags(uint8_t value)
{
    cpu.status &= (uint8_t)~(FLAG_Z | FLAG_N);
    if (value == 0)   cpu.status |= FLAG_Z;
    if (value & 0x80) cpu.status |= FLAG_N;
}

static uint16_t read_operand(void)
{
    uint8_t low = bus_read(cpu.pc++);
    uint8_t high = bus_read(cpu.pc++);
    return (uint16_t)(low | (high << 8));
}

static void cpu_reset(void)
{
    cpu.a = cpu.x = cpu.y = 0;
    cpu.sp = 0xFD;
    cpu.status = FLAG_I | FLAG_U;
    cpu.cycles = 0;
    cpu.pc = (uint16_t)(bus_read(0xFFFC) | (bus_read(0xFFFD) << 8));
}

/* One instruction. The tick count starts at two for every instruction and each
 * case adds the ticks that instruction really takes on the console's chip: a
 * read from memory costs one more, a write to the cartridge two, a branch that
 * is taken one. A real emulator keeps these numbers in a table because they are
 * the same for every program; here they sit next to the work they pay for,
 * which is where you can check them.
 *
 * Why this matters at all: the emulator must run the game for a whole frame of
 * the console's time, and the only measure of that time is this counter. The
 * instruction count is not it — an instruction takes two ticks or six, and a
 * program that waits in a loop still burns the frame.
 */
static bool cpu_step(void)
{
    uint8_t  op = bus_read(cpu.pc++);
    uint16_t adr;
    uint8_t  value;

    cpu.cycles += TICKS_PER_INSTRUCTION;

    switch (op) {
    case 0x00:                                    /* brk: stop for good */
        cpu.cycles += 5;
        return false;

    /* ---- load and store ------------------------------------------- */
    case 0xA9: cpu.a = bus_read(cpu.pc++); set_flags(cpu.a); break;
    case 0xA5: cpu.cycles += 1;
               cpu.a = bus_read(bus_read(cpu.pc++)); set_flags(cpu.a); break;
    case 0xAD: cpu.cycles += 2;
               cpu.a = bus_read(read_operand()); set_flags(cpu.a); break;
    case 0xBD: cpu.cycles += 2;
               adr = (uint16_t)(read_operand() + cpu.x);
               cpu.a = bus_read(adr); set_flags(cpu.a); break;
    case 0xB9: cpu.cycles += 2;
               adr = (uint16_t)(read_operand() + cpu.y);
               cpu.a = bus_read(adr); set_flags(cpu.a); break;
    case 0xB1: {                                  /* lda (pointer),y */
        uint8_t zero = bus_read(cpu.pc++);
        uint16_t base = (uint16_t)(bus_read(zero)
                                   | (bus_read((uint8_t)(zero + 1)) << 8));
        cpu.cycles += 3;
        cpu.a = bus_read((uint16_t)(base + cpu.y));
        set_flags(cpu.a);
        break;
    }
    case 0xA2: cpu.x = bus_read(cpu.pc++); set_flags(cpu.x); break;
    case 0xA6: cpu.cycles += 1;
               cpu.x = bus_read(bus_read(cpu.pc++)); set_flags(cpu.x); break;
    case 0xA0: cpu.y = bus_read(cpu.pc++); set_flags(cpu.y); break;
    case 0xA4: cpu.cycles += 1;
               cpu.y = bus_read(bus_read(cpu.pc++)); set_flags(cpu.y); break;
    case 0x85: cpu.cycles += 1; bus_write(bus_read(cpu.pc++), cpu.a); break;
    case 0x8D: cpu.cycles += 2; bus_write(read_operand(), cpu.a); break;
    case 0x91: {                                  /* sta (pointer),y */
        uint8_t zero = bus_read(cpu.pc++);
        uint16_t base = (uint16_t)(bus_read(zero)
                                   | (bus_read((uint8_t)(zero + 1)) << 8));
        cpu.cycles += 4;
        bus_write((uint16_t)(base + cpu.y), cpu.a);
        break;
    }

    /* ---- move a byte between registers ---------------------------- */
    case 0xAA: cpu.x = cpu.a; set_flags(cpu.x); break;
    case 0x8A: cpu.a = cpu.x; set_flags(cpu.a); break;
    case 0xA8: cpu.y = cpu.a; set_flags(cpu.y); break;
    case 0x98: cpu.a = cpu.y; set_flags(cpu.a); break;
    case 0x9A: cpu.sp = cpu.x; break;

    /* ---- count ---------------------------------------------------- */
    case 0xE8: cpu.x++; set_flags(cpu.x); break;
    case 0xC8: cpu.y++; set_flags(cpu.y); break;
    case 0xCA: cpu.x--; set_flags(cpu.x); break;
    case 0x88: cpu.y--; set_flags(cpu.y); break;
    case 0xE6: {                                  /* inc in page zero */
        uint8_t zero = bus_read(cpu.pc++);
        cpu.cycles += 3;
        value = (uint8_t)(bus_read(zero) + 1);
        bus_write(zero, value);
        set_flags(value);
        break;
    }
    case 0xEE: {                                  /* inc anywhere */
        adr = read_operand();
        cpu.cycles += 4;
        value = (uint8_t)(bus_read(adr) + 1);
        bus_write(adr, value);
        set_flags(value);
        break;
    }

    /* ---- arithmetic ----------------------------------------------- */
    case 0x69: {                                  /* adc immediate */
        uint8_t carry = (uint8_t)(cpu.status & FLAG_C);
        uint16_t sum = (uint16_t)(cpu.a + bus_read(cpu.pc++) + carry);
        cpu.status &= (uint8_t)~(FLAG_C | FLAG_V);
        if (sum > 0xFF) cpu.status |= FLAG_C;
        if (~(cpu.a ^ (uint8_t)sum) & (cpu.a ^ (uint8_t)(sum >> 8)) & 0x80)
            cpu.status |= FLAG_V;
        cpu.a = (uint8_t)sum;
        set_flags(cpu.a);
        break;
    }
    case 0x29: cpu.a &= bus_read(cpu.pc++); set_flags(cpu.a); break;
    case 0x49: cpu.a ^= bus_read(cpu.pc++); set_flags(cpu.a); break;

    /* ---- compare: a subtraction whose result is thrown away ------- */
    case 0xC9: {
        value = bus_read(cpu.pc++);
        cpu.status &= (uint8_t)~FLAG_C;
        if (cpu.a >= value) cpu.status |= FLAG_C;
        set_flags((uint8_t)(cpu.a - value));
        break;
    }
    case 0xE0: {
        value = bus_read(cpu.pc++);
        cpu.status &= (uint8_t)~FLAG_C;
        if (cpu.x >= value) cpu.status |= FLAG_C;
        set_flags((uint8_t)(cpu.x - value));
        break;
    }
    case 0xC0: {
        value = bus_read(cpu.pc++);
        cpu.status &= (uint8_t)~FLAG_C;
        if (cpu.y >= value) cpu.status |= FLAG_C;
        set_flags((uint8_t)(cpu.y - value));
        break;
    }

    /* ---- branches: a signed distance from the next instruction ---- */
    case 0x10: case 0x30: case 0xD0: case 0xF0: {
        int8_t distance = (int8_t)bus_read(cpu.pc++);
        bool take;
        if      (op == 0x10) take = !(cpu.status & FLAG_N);
        else if (op == 0x30) take =  (cpu.status & FLAG_N) != 0;
        else if (op == 0xD0) take = !(cpu.status & FLAG_Z);
        else                 take =  (cpu.status & FLAG_Z) != 0;
        if (take) {
            cpu.cycles += 1;                      /* a branch taken costs more */
            cpu.pc = (uint16_t)(cpu.pc + distance);
        }
        break;
    }

    /* ---- go somewhere else, and come back ------------------------- */
    case 0x4C: cpu.cycles += 1; cpu.pc = read_operand(); break;
    case 0x20: {                                  /* jsr */
        uint16_t target = read_operand();
        uint16_t back = (uint16_t)(cpu.pc - 1);
        cpu.cycles += 4;
        bus_write((uint16_t)(0x0100 + cpu.sp--), (uint8_t)(back >> 8));
        bus_write((uint16_t)(0x0100 + cpu.sp--), (uint8_t)(back & 0xFF));
        cpu.pc = target;
        break;
    }
    case 0x60: {                                  /* rts */
        uint8_t low  = bus_read((uint16_t)(0x0100 + ++cpu.sp));
        uint8_t high = bus_read((uint16_t)(0x0100 + ++cpu.sp));
        cpu.cycles += 4;
        cpu.pc = (uint16_t)((low | (high << 8)) + 1);
        break;
    }

    /* ---- flags ---------------------------------------------------- */
    case 0x18: cpu.status &= (uint8_t)~FLAG_C; break;
    case 0xD8: cpu.status &= (uint8_t)~FLAG_D; break;
    case 0x58: cpu.status &= (uint8_t)~FLAG_I; break;
    case 0x78: cpu.status |= FLAG_I; break;
    case 0x24: case 0x2C: {                       /* bit: look without taking */
        adr = (op == 0x24) ? bus_read(cpu.pc++) : read_operand();
        cpu.cycles += (op == 0x24) ? 1 : 2;
        value = bus_read(adr);
        cpu.status &= (uint8_t)~(FLAG_Z | FLAG_N);
        if ((cpu.a & value) == 0) cpu.status |= FLAG_Z;
        cpu.status |= (uint8_t)(value & FLAG_N);
        break;
    }
    case 0xEA: break;                             /* nop */

    default:
        stopped_at = (uint16_t)(cpu.pc - 1);
        stopped_opcode = op;
        return false;
    }
    return true;
}

/* --------------------------------------------------------- the picture chip
 * Video memory, the tile shapes, the palette and the six registers, as in
 * stages 08 and 13. One thing is new: every write into video memory is noted
 * down, so the emulator can later ask which bands of the picture the game has
 * touched. Band here means eight pixel rows — one row of tiles.
 */
static uint8_t vram[TILE_ROWS * TILE_COLS];
static uint8_t chr[CHR_BYTES];
static uint8_t palette[4];
static uint16_t ppu_address;
static bool     ppu_address_high;
static uint8_t  ppu_scroll_x, ppu_scroll_y;
static uint8_t  ppu_mask;
static uint8_t  ppu_status;

/* One flag per band: something in it changed since the picture was last drawn. */
static uint8_t band_changed[TILE_ROWS];

static void ppu_all_bands_changed(void)
{
    for (int i = 0; i < TILE_ROWS; i++) band_changed[i] = 1;
}

static void ppu_reset(void)
{
    memset(vram, 0, sizeof vram);
    memset(chr, 0, sizeof chr);
    memset(palette, 0, sizeof palette);
    ppu_address = 0;
    ppu_address_high = false;
    ppu_scroll_x = ppu_scroll_y = 0;
    ppu_mask = ppu_status = 0;
    ppu_all_bands_changed();
}

/* A frame has been drawn: the flag the game's loop spins on. On the console the
 * picture chip raises it by itself; here the emulator decides where the frame
 * ends, because it does not emulate the chip tick by tick. */
static void ppu_start_frame(void) { ppu_status |= 0x80; }

/* Forget which bands changed, once they have been drawn and sent. */
static void ppu_frame_drawn(void)
{
    for (int i = 0; i < TILE_ROWS; i++) band_changed[i] = 0;
}

static uint8_t ppu_read(uint16_t addr)
{
    if (addr == 0x2002) {
        uint8_t value = ppu_status;
        ppu_status &= (uint8_t)~0x80;      /* reading it clears the flag */
        return value;
    }
    return 0;
}

static void ppu_write(uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0x2001:
        ppu_mask = value;
        ppu_all_bands_changed();
        break;

    case 0x2005:
        /* Two writes: how far right, then how far down. Scrolling moves where
         * the walk over the picture starts, so every band shows something
         * else — which is why the whole screen counts as changed. */
        if (!ppu_address_high) ppu_scroll_x = value;
        else                   ppu_scroll_y = value;
        ppu_address_high = !ppu_address_high;
        ppu_all_bands_changed();
        break;

    case 0x2006:
        if (!ppu_address_high) ppu_address = (uint16_t)((ppu_address & 0x00FF) | (value << 8));
        else                   ppu_address = (uint16_t)((ppu_address & 0xFF00) | value);
        ppu_address &= 0x3FFF;
        ppu_address_high = !ppu_address_high;
        break;

    case 0x2007:
        if (ppu_address < 0x2000) {
            chr[ppu_address % CHR_BYTES] = value;          /* a tile shape */
            ppu_all_bands_changed();
        } else if (ppu_address < 0x3F00) {
            uint16_t cell = (uint16_t)((ppu_address - 0x2000) % (TILE_ROWS * TILE_COLS));
            vram[cell] = value;
            band_changed[cell / TILE_COLS] = 1;            /* only this band */
        } else {
            palette[(ppu_address - 0x3F00) % 4] = (uint8_t)(value & 0x07);
            ppu_all_bands_changed();
        }
        ppu_address = (uint16_t)((ppu_address + 1) & 0x3FFF);
        break;

    default:
        break;                                  /* no other register here */
    }
}

/* The picture: one byte per pixel, holding a colour number. */
static uint8_t framebuffer[PPU_W * PPU_H];

/* The two bytes of every pixel, in the order the panel wants them. There is no
 * panel on this computer, so the picture stops here: filling this buffer is the
 * work the sending loop does on the board, and that is the work being timed. */
static uint16_t wire[PPU_W * PPU_H];

/* ------------------------------------------------------- drawing the picture
 * Three ways to turn video memory into pixels. The first is the code of stage
 * 13: for every pixel it works out which column of the world that pixel shows,
 * with a remainder, and then divides that column by eight to land in a tile —
 * 61,440 remainders and twice as many divisions in a frame. The second walks
 * the world instead of computing it. The third walks it only where something
 * changed.
 */
static void render_pixels(void)
{
    for (int y = 0; y < PPU_H; y++) {
        int world_y = (y + ppu_scroll_y) % WORLD_H;   /* once per row */
        for (int x = 0; x < PPU_W; x++) {
            int world_x = (x + ppu_scroll_x) % WORLD_W;
            uint8_t tile = vram[(world_y / TILE) * TILE_COLS + world_x / TILE];
            const uint8_t *shape = &chr[tile * 16 + (world_y % TILE)];
            unsigned bit = 7u - (unsigned)(world_x % TILE);
            uint8_t value = (uint8_t)(((shape[0] >> bit) & 1u)
                                    | (((shape[8] >> bit) & 1u) << 1));
            framebuffer[y * PPU_W + x] = palette[value];
        }
    }
}

/* One pixel row of the world: 256 pixels starting at world column world_x.
 * Nothing here divides. The column index walks forward, and the tile it lands
 * in changes once every eight pixels, which is a compare and not a remainder. */
static void render_world_row(int tile_row, int shape_row, int world_x, uint8_t *out)
{
    const uint8_t *tiles = &vram[tile_row * TILE_COLS];
    int bit = 7 - (world_x & 7);                /* first tile may be half over */

    for (int x = 0; x < PPU_W; x++) {
        uint8_t tile = tiles[world_x >> 3];
        const uint8_t *shape = &chr[tile * 16 + shape_row];
        *out++ = palette[(uint8_t)(((shape[0] >> bit) & 1u)
                                 | (((shape[8] >> bit) & 1u) << 1))];
        if (bit == 0) {                         /* that tile is used up */
            bit = 7;
            world_x += TILE;
            /* Scrolled by less than a whole tile, the walk starts inside one,
             * so stepping tile by tile lands past the end instead of exactly
             * on it. The wrap has to catch that, or the last pixels of a row
             * come from the row below. */
            if (world_x >= WORLD_W) world_x -= WORLD_W;
        } else {
            bit--;
        }
    }
}

/* Walk the picture band by band. A band is eight pixel rows, and the world row
 * it starts on moves forward by one per pixel row, wrapping at the bottom. */
static void render_walk(bool only_changed)
{
    int tile_row  = ppu_scroll_y / TILE;        /* two divisions per frame, */
    int shape_row = ppu_scroll_y % TILE;        /* not per pixel            */
    int top = 0;

    for (int band = 0; band < TILE_ROWS; band++) {
        bool draw = !only_changed || band_changed[band];
        for (int row = 0; row < TILE; row++) {
            /* A band that did not change is left as it is: the panel already
             * holds those pixels. Its rows still have to be counted, or every
             * band below it would show the wrong part of the world. */
            if (draw) {
                render_world_row(tile_row, shape_row, ppu_scroll_x,
                                 &framebuffer[top * PPU_W]);
            }
            top++;
            if (++shape_row == TILE) {
                shape_row = 0;
                if (++tile_row == TILE_ROWS) tile_row = 0;
            }
        }
    }
}

static void render_picture(bool walk, bool only_changed)
{
    /* Told to show no background, the chip puts one colour on the whole screen.
     * That counts as every band changed, because the bands below hold something
     * else and the panel has to hear about it. */
    if (!(ppu_mask & 0x08)) {
        memset(framebuffer, palette[0], sizeof framebuffer);
        ppu_all_bands_changed();
        return;
    }

    if (walk) render_walk(only_changed);
    else      render_pixels();
}

/* ------------------------------------------------------ the panel's two bytes
 * The straightforward version builds the two bytes of every pixel out of the
 * colour number: one look-up for the colour, two shifts to put its bytes in the
 * order the panel wants. The other one looks the finished word up, and skips
 * the bands that did not change: the panel still holds those, so sending them
 * again would put the same bytes on the same wires.
 */
static void push_bytes(void)
{
    for (int i = 0; i < PPU_W * PPU_H; i++) {
        uint16_t colour = panel_colours[framebuffer[i]];
        wire[i] = (uint16_t)((colour >> 8) | (colour << 8));   /* high byte first */
    }
}

static void push_words(bool only_changed)
{
    for (int band = 0; band < TILE_ROWS; band++) {
        if (only_changed && !band_changed[band]) continue;
        int first = band * TILE * PPU_W;
        for (int i = 0; i < TILE * PPU_W; i++)
            wire[first + i] = panel_words[framebuffer[first + i]];
    }
}

static void push_picture(bool words, bool only_changed)
{
    if (words) push_words(only_changed);
    else       push_bytes();
}

/* ------------------------------------------------------------- the machine */
uint8_t bus_read(uint16_t addr)
{
    if (addr < 0x2000) return work_ram[addr & 0x07FF];   /* 2 KB, mirrored */
    if (addr < 0x4000) return ppu_read((uint16_t)(0x2000 + (addr & 7)));
    if (addr >= 0x8000) return cart_rom[addr - 0x8000];
    return 0;                                            /* nothing is wired */
}

void bus_write(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) {
        work_ram[addr & 0x07FF] = value;
    } else if (addr < 0x4000) {
        ppu_write((uint16_t)(0x2000 + (addr & 7)), value);
    }
    /* writes into the cartridge are ignored: this cartridge has no bank
     * register to switch, unlike the one in stage 13 */
}

static void machine_reset(void)
{
    memset(work_ram, 0, sizeof work_ram);
    ppu_reset();
    cpu_reset();
}

/* Run the game for one frame of the console's time: not one frame of the
 * game's, but exactly as many ticks as the console would have given it. The
 * loop is what the counter is for. The last instruction may overshoot the
 * boundary by a few ticks, and that is what the real machine does too. */
static uint32_t run_game_frame(void)
{
    cpu.cycles = 0;
    while (cpu.cycles < CYCLES_PER_FRAME) {
        if (!cpu_step()) break;
    }
    ppu_start_frame();
    return cpu.cycles;
}

/* --------------------------------------------------------------- measurement
 * clock() counts the processor time this program has used, in units of
 * CLOCKS_PER_SEC. It is the C library's stopwatch: nothing to configure, and
 * no need to know how the computer's clock is built. Everything the table says
 * about where the time goes comes from these two calls around each phase.
 *
 * Two words of warning, because a measurement is only worth what it is worth.
 * The computer measures time in steps, so a phase shorter than one step comes
 * out lumpy, and another program taking the processor now and then adds time
 * that has nothing to do with the emulator. The answer to both is to measure
 * the same thing several times and keep the fastest run: interference can only
 * make a run slower, never faster. Three runs leave the middle of the table
 * wobbling by a third; six settle it.
 */
#define PHASE_COUNT 3
#define CONFIG_COUNT 4
#define REPEATS 6
#define PHASE_CPU    0
#define PHASE_RENDER 1
#define PHASE_PANEL  2

static const char *phase_name[PHASE_COUNT] = { "processor", "drawing", "panel" };
static const char *config_name[CONFIG_COUNT] = {
    "as it was", "+walk", "+words", "+changed bands",
};

static double   spent[PHASE_COUNT];
static double   results[CONFIG_COUNT][PHASE_COUNT];
static uint32_t picture_hash[CONFIG_COUNT];
static uint32_t wire_hash[CONFIG_COUNT];
static uint32_t cycles_run;

static clock_t phase_started;

static void phase_begin(void) { phase_started = clock(); }

static void phase_end(int phase)
{
    spent[phase] += (double)(clock() - phase_started);
}

static uint32_t checksum(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Emulate the same frames once, with the changes this configuration asks for. */
static void run(int config, bool walk, bool words, bool only_changed)
{
    memset(spent, 0, sizeof spent);
    cycles_run = 0;
    machine_reset();

    for (int frame = 0; frame < FRAMES; frame++) {
        phase_begin();
        cycles_run += run_game_frame();
        phase_end(PHASE_CPU);

        phase_begin();
        render_picture(walk, only_changed);
        phase_end(PHASE_RENDER);

        phase_begin();
        push_picture(words, only_changed);
        phase_end(PHASE_PANEL);

        ppu_frame_drawn();
    }

    picture_hash[config] = checksum(framebuffer, sizeof framebuffer);
    wire_hash[config] = checksum(wire, sizeof wire);
}

/* And now as many times as it takes, keeping the fastest run of each phase. */
static void measure(int config, bool walk, bool words, bool only_changed)
{
    for (int p = 0; p < PHASE_COUNT; p++) results[config][p] = 1e30;

    for (int repeat = 0; repeat < REPEATS; repeat++) {
        run(config, walk, words, only_changed);
        for (int p = 0; p < PHASE_COUNT; p++) {
            if (spent[p] < results[config][p]) results[config][p] = spent[p];
        }
    }
}

static double milliseconds(int config, int phase)
{
    return results[config][phase] * 1000.0 / CLOCKS_PER_SEC / FRAMES;
}

static double total_milliseconds(int config)
{
    double total = 0.0;
    for (int p = 0; p < PHASE_COUNT; p++) total += milliseconds(config, p);
    return total;
}

static void print_table(void)
{
    printf("%d frames, one console frame = %u processor ticks\n\n",
           FRAMES, (unsigned)CYCLES_PER_FRAME);

    printf("%-14s", "milliseconds");
    for (int c = 0; c < CONFIG_COUNT; c++) printf("%16s", config_name[c]);
    printf("\n");

    for (int p = 0; p < PHASE_COUNT; p++) {
        printf("%-14s", phase_name[p]);
        for (int c = 0; c < CONFIG_COUNT; c++) printf("%16.3f", milliseconds(c, p));
        printf("\n");
    }

    printf("%-14s", "one frame");
    for (int c = 0; c < CONFIG_COUNT; c++) printf("%16.3f", total_milliseconds(c));
    printf("\n");

    printf("%-14s", "frames/second");
    for (int c = 0; c < CONFIG_COUNT; c++)
        printf("%16.0f", 1000.0 / total_milliseconds(c));
    printf("\n");

    printf("\neach configuration was emulated %d times; the fastest run is above.\n",
           REPEATS);
    printf("the console allows 16.667 ms for a frame; the last one uses %.3f ms of it\n",
           total_milliseconds(CONFIG_COUNT - 1));

    printf("average %lu ticks per frame, against %u the console has\n",
           (unsigned long)(cycles_run / FRAMES), (unsigned)CYCLES_PER_FRAME);

    if (stopped_opcode)
        printf("the processor stopped at $%04X: opcode $%02X is not implemented\n",
               stopped_at, stopped_opcode);

    printf("picture checksum");
    for (int c = 0; c < CONFIG_COUNT; c++) printf("%16.8lX", (unsigned long)picture_hash[c]);
    printf("\nwires checksum  ");
    for (int c = 0; c < CONFIG_COUNT; c++) printf("%16.8lX", (unsigned long)wire_hash[c]);
    printf("\n");
}

/* One pixel out of every 64, enough to see the landscape and the bird. The
 * sample sits in the middle of a tile rather than at its corner, where the top
 * edge of a grass tile would be the only thing on show. */
static void print_picture(void)
{
    printf("\nthe picture, one pixel out of every 64:\n");
    for (int y = TILE / 2; y < PPU_H; y += TILE) {
        for (int x = TILE / 2; x < PPU_W; x += TILE)
            putchar(colour_glyph[framebuffer[y * PPU_W + x] & (COLOUR_COUNT - 1)]);
        putchar('\n');
    }
}

int main(void)
{
    load_cartridge();
    build_panel_words();

    /* One run before anything is measured: the first pass over a page of memory
     * is slower than the rest, and a cold start has nothing to do with the
     * emulator. Its result is thrown away. */
    run(0, false, false, false);

    measure(0, false, false, false);      /* the code of stage 13          */
    measure(1, true,  false, false);      /* and the walk without division */
    measure(2, true,  true,  false);      /* and the ready-made panel bytes */
    measure(3, true,  true,  true);       /* and only the bands that moved */

    print_table();
    print_picture();

    printf("\nthese times come from this computer; yours will differ\n");
    return 0;
}
