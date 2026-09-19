/*
 * Stage 6 — memory and the bus, taught on a processor that reads its own code.
 *
 * Stage 05 gave us a processor that could only work on numbers it already held in
 * its registers. A real processor does not keep a program inside itself: it asks
 * for every instruction and every number by address. That request path is the
 * bus, and this stage builds one:
 *
 *   memory      one long array of bytes; the machine's whole address space
 *   an address  the number of one byte in that array
 *   the bus     a read function and a write function; every access goes through
 *               them, so a device can answer in place of the array
 *   the map     which range of addresses belongs to what
 *
 * The last part is the one that matters for the emulator. A few addresses in the
 * map are not memory at all: they are registers, places that answer with a fresh
 * number or change something when written. Reading the same address twice gives
 * two different answers, and no array ever does that.
 *
 * This is a program for your computer, not for the board: no desk hardware works
 * quite like the console. Build it with
 *
 *     cc -Wall -Wextra main.c -o etap06
 *
 * The panel and the board come back in stage 08.
 */
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------ the address space
 * One array stands in for the whole space of addresses the processor can reach. A
 * real machine does not have one array: it has several chips and devices wired to
 * the same wires, and the address decides who answers. Here the array answers for
 * memory, and two addresses above it are answered by the bus functions instead.
 */
#define MEM_SIZE 32768u             /* 32 KB of addresses, 0x0000 to 0x7FFF */

/*
 * The memory map — what lives where. Addresses are written in hexadecimal, the
 * same way the program and the dump show them.
 *
 *   0x0000   the program being executed
 *   0x0010   two bytes holding the address a program reads its number through
 *   0x0020   the number program one works on
 *   0x0030   where program two leaves its answer
 *   0x0080   counter, a device register: it answers, then counts down
 *   0x0081   result, a device register: it keeps what was written to it
 *   0x7FFF   the last byte of memory: the array stops here
 *
 * Instructions and numbers share one space, so they need different addresses. The
 * map is how everybody agrees on which is which. Nothing enforces it: a program
 * that writes outside its own data will happily eat its own code.
 */
#define PROG_BASE 0x0000u           /* every program is loaded here */
#define THROUGH   0x0010u           /* the two bytes holding an address */
#define DATA1     0x0020u           /* the number program one reads */
#define ANSWER    0x0030u           /* where program two leaves its answer */
#define REG_COUNTER 0x0080u         /* answers, then counts down, once per read */
#define REG_RESULT  0x0081u         /* keeps the last number written to it */
#define REG_FIRST   0x0080u         /* the bus hands this address and the next */
#define REG_LAST    0x0081u         /* to a device instead of to the array */

static uint8_t mem[MEM_SIZE];
static uint8_t counter;             /* what REG_COUNTER answers with next */
static uint8_t result;              /* what was last written to REG_RESULT */

/* ------------------------------------------------------------------- the bus
 * Two functions, and nothing else in the machine touches mem. That is the whole
 * trick of a bus: the processor does not know whether a number comes from an
 * array, from a chip, or from a device that made it up on the spot. It asks for
 * an address and gets a byte back.
 *
 * The check for a bad address matters for the same reason. Nothing stops a
 * program from asking for 0x9000, and the answer has to be something the program
 * can survive instead of a crash of the whole machine.
 */
static uint8_t mem_read(uint16_t addr)
{
    if (addr >= REG_FIRST && addr <= REG_LAST) {
        uint8_t answer = counter;   /* the device answers with what it holds, */
        counter--;                  /* and the read itself counts it down */
        return answer;
    }
    if (addr >= MEM_SIZE) {
        fprintf(stderr, "read outside memory: 0x%04X\n", (unsigned)addr);
        return 0;
    }
    return mem[addr];
}

static void mem_write(uint16_t addr, uint8_t value)
{
    if (addr >= REG_FIRST && addr <= REG_LAST) {
        result = value;             /* the number never lands in the array */
        return;
    }
    if (addr >= MEM_SIZE) {
        fprintf(stderr, "write outside memory: 0x%04X\n", (unsigned)addr);
        return;
    }
    mem[addr] = value;
}

/* ------------------------------------------------------------- the processor
 * Same shape as stage 05: a program counter, one register to work in, and a loop
 * that runs one instruction per turn. The new part is that nothing is handed to
 * the processor directly any more. The instruction, and the number it works on,
 * both arrive through the bus.
 *
 * An instruction is two bytes: what to do, and where to do it. The second byte is
 * a small address, so it can name any byte from 0x0000 to 0x00FF, which is where
 * both programs here keep everything they touch.
 *
 * Every argument is an address in this machine, even the constant in LDA. The
 * processor reads the byte holding the constant exactly as it reads a byte
 * holding a variable; the difference between the two is only in what the program
 * does with them.
 */
#define OP_HLT 0x00                 /* stop */
#define OP_LDA 0x01                 /* A = byte at address */
#define OP_STA 0x02                 /* byte at address = A */
#define OP_ADC 0x03                 /* A = A + byte at address */
#define OP_INC 0x04                 /* byte at address = byte at address + 1 */

/* One argument is special: 0xFF means "not an address itself, but the address
 * stored at 0x0010". Program two uses it to read the same place through a number
 * that sits in memory, which is how a program can be pointed at a device without
 * changing a single instruction. */
#define ARG_INDIRECT 0xFF

struct cpu {
    uint16_t pc;                    /* program counter: next instruction */
    uint8_t  a;                     /* the one register to work in */
    int      halted;                /* set when HLT ran */
    long     steps;                 /* run so far; the limit stops endless loops */
};

/* Work out where an instruction's argument really points. */
static uint16_t target(uint8_t arg)
{
    if (arg != ARG_INDIRECT) {
        return arg;
    }
    return (uint16_t)(mem[THROUGH] | (mem[THROUGH + 1] << 8));
}

/* Start the processor from the bottom of memory, with an empty accumulator. The
 * reset is not a formality: a machine that keeps the number left in its register
 * runs the next program with somebody else's data. */
static void run(struct cpu *c, long limit)
{
    c->pc = PROG_BASE;
    c->a = 0;
    c->halted = 0;
    c->steps = 0;

    while (c->steps < limit) {
        uint16_t here = c->pc;              /* where this instruction starts */
        uint8_t op = mem_read(c->pc);       /* fetch the instruction, by address */
        uint8_t arg = mem_read(c->pc + 1);  /* then its argument */
        c->pc = (uint16_t)(c->pc + 2);      /* two bytes eaten */
        c->steps++;

        if (op == OP_HLT) {
            c->halted = 1;
            return;
        } else if (op == OP_LDA) {
            c->a = mem_read(target(arg));
        } else if (op == OP_STA) {
            mem_write(target(arg), c->a);
        } else if (op == OP_ADC) {
            c->a = (uint8_t)(c->a + mem_read(target(arg)));
        } else if (op == OP_INC) {
            uint16_t at = target(arg);
            mem_write(at, (uint8_t)(mem_read(at) + 1));
        } else {
            /* An instruction nobody knows. On a real machine this is where a
             * program dies, and the address is the only clue you get. */
            printf("unknown instruction 0x%02X at 0x%04X\n",
                   (unsigned)op, (unsigned)here);
            c->halted = 1;
            return;
        }
    }
}

/* ---------------------------------------------------------------- program one
 * Program one reads one number twice, and changes it in between.
 *
 *   0x0000  LDA  0x20     A = the byte at 0x0020, which is 5
 *   0x0002  INC  0x20     the byte at 0x0020 = that byte + 1, so now 6
 *   0x0004  ADC  0x20     A = A + the byte at 0x0020, that is 5 + 6 = 11
 *   0x0006  STA  0x81     the result register = A
 *   0x0008  HLT  0x00     stop; HLT ignores its argument
 *
 * INC changes a byte that another instruction reads. That is the whole of
 * self-modifying code, and it needs no special machinery: the number arrives
 * through the same bus as everything else, so INC has no idea whether it is
 * changing a variable or a piece of the program.
 *
 * Walk it through with the dump in front of you. LDA fetches 5. INC makes it 6.
 * ADC reads the same address again and finds 6, so the sum is 11 and not 10. Two
 * reads of one address, two answers — and this time for an honest reason: the
 * program itself changed the number in between.
 */
#define PROG1_SIZE 10
static const uint8_t prog1[PROG1_SIZE] = {
    OP_LDA, 0x20,               /* 0x0000: A = the byte at 0x0020 */
    OP_INC, 0x20,               /* 0x0002: that byte = that byte + 1 */
    OP_ADC, 0x20,               /* 0x0004: A = A + that byte */
    OP_STA, 0x81,               /* 0x0006: the result register = A */
    OP_HLT, 0x00,               /* 0x0008: stop */
};

/* ---------------------------------------------------------------- program two
 * Program two reads one number twice, without changing anything itself.
 *
 *   0x0000  LDA  0xFF     A = the byte at the address held in 0x0010
 *   0x0002  ADC  0xFF     A = A + that same byte again
 *   0x0004  STA  0x30     the byte at 0x0030 = A
 *   0x0006  HLT  0x00     stop
 *
 * The program never changes: not one instruction, not one address. What changes
 * is the pair of bytes at 0x0010, which holds an address. Point them at 0x0020
 * and the two reads come back with the same number twice. Point them at 0x0080
 * and the two reads land on the counter, which answers once and then answers
 * again with one less — and that is the difference between memory and a register.
 */
#define PROG2_SIZE 8
static const uint8_t prog2[PROG2_SIZE] = {
    OP_LDA, ARG_INDIRECT,       /* 0x0000: A = the byte at the address in 0x0010 */
    OP_ADC, ARG_INDIRECT,       /* 0x0002: A = A + that same byte */
    OP_STA, 0x30,               /* 0x0004: the byte at 0x0030 = A */
    OP_HLT, 0x00,               /* 0x0006: stop */
};

/* -------------------------------------------------------------------- loading
 * The loader puts a program at PROG_BASE, which is where the processor starts
 * looking, and clears the rest of memory. That clearing is not a detail: a
 * machine that comes up full of whatever was left behind runs a different program
 * every time you switch it on.
 */
static void load(const uint8_t *program, int size)
{
    for (int i = 0; i < (int)MEM_SIZE; i++) {
        mem[i] = 0;
    }
    for (int i = 0; i < size; i++) {
        mem[PROG_BASE + i] = program[i];
    }
}

/* Put a two-byte address at 0x0010, the small half first. That is the order the
 * processor reads it back in, and it is the order every machine that stores an
 * address in memory uses: the halves have to agree on which comes first. */
static void point_at(uint16_t address)
{
    mem[THROUGH]     = (uint8_t)address;
    mem[THROUGH + 1] = (uint8_t)(address >> 8);
}

/* -------------------------------------------------------------------- report
 * A memory dump, the way a debugger shows it: sixteen bytes to a row, each row
 * headed by the address of its first byte. Those addresses are what turn a heap
 * of numbers into a map.
 */
static void dump(const char *title, int from, int bytes)
{
    printf("%s\n\n", title);
    for (int row = 0; row < bytes; row += 16) {
        printf("  %04X: ", (unsigned)(from + row));
        for (int i = 0; i < 16 && row + i < bytes; i++) {
            printf("%02X ", (unsigned)mem[from + row + i]);
        }
        printf("\n");
    }
    printf("\n");
}

int main(void)
{
    struct cpu c = { 0, 0, 0, 0 };
    uint8_t first = 0;                  /* program one's number, kept for the report */
    uint8_t start = 0;                  /* what the counter answers first */

    /* ------------------------------------------------ one: the bus and the map */
    printf("Demo 1: a program that changes its own data\n\n");

    counter = 0;
    result  = 0;
    load(prog1, PROG1_SIZE);
    mem[DATA1] = 5;                     /* the number the program works on */
    first = mem[DATA1];                 /* the lines below quote it back */

    dump("The machine before the run: the program at 0x0000, its number at 0x0020.",
         PROG_BASE, 48);

    run(&c, 1000);
    printf("A after the run:  %u\n", (unsigned)c.a);
    printf("result register:  0x%04X\n", (unsigned)result);
    printf("instructions run: %ld\n", c.steps);
    printf("program halted:   %s\n\n", c.halted ? "yes" : "no");
    printf("0x0020 held %u and now holds %u, because INC wrote to it.\n",
           (unsigned)first, (unsigned)mem[DATA1]);
    printf("LDA saw the first number and ADC saw the second, so the sum is %u.\n\n",
           (unsigned)(first + mem[DATA1]));
    dump("The same memory afterwards:", PROG_BASE, 48);
    printf("Compare the second row with the first. The bytes of the program at\n");
    printf("0x0000 are the same ten bytes. The byte at 0x0020 went from %02X to %02X,\n",
           (unsigned)first, (unsigned)mem[DATA1]);
    printf("because INC wrote to it, and that single byte is the whole difference\n");
    printf("between the sum being %u and the sum being %u.\n\n",
           (unsigned)(first + mem[DATA1]), (unsigned)(2 * first));

    /* ---------------------------------------- two: a device, and memory beside it */
    printf("Demo 2: two reads of one address in plain memory\n\n");

    counter = 3;
    result  = 0;
    load(prog2, PROG2_SIZE);
    mem[DATA1] = 7;                     /* ordinary memory, holding 7 */
    mem[ANSWER] = 0;
    point_at(DATA1);                    /* 0x0010 says: read 0x0020 */

    run(&c, 1000);
    printf("0x0020 still holds %u, because nothing wrote to it.\n",
           (unsigned)mem[DATA1]);
    printf("A after the run:  %u   (the same number on every read)\n",
           (unsigned)c.a);
    printf("and the answer left at 0x0030 is %u\n\n", (unsigned)mem[ANSWER]);

    printf("Demo 3: the same program, with the counter underneath\n\n");

    counter = 3;                        /* the device starts counting at three */
    start   = counter;                  /* the first read answers with this */
    result  = 0;
    load(prog2, PROG2_SIZE);
    mem[DATA1] = 0;                     /* nothing in memory matters this time */
    mem[ANSWER] = 0;
    point_at(REG_COUNTER);              /* 0x0010 says: read 0x0080 */

    run(&c, 1000);
    printf("the counter answered with %u and then with %u, one less each time\n",
           (unsigned)start, (unsigned)(start - 1));
    printf("so reads of one address added up to %u\n", (unsigned)c.a);
    printf("and the answer left at 0x0030 is %u\n\n", (unsigned)mem[ANSWER]);
    printf("Same program, same instruction, same address 0x0010. Only the two\n");
    printf("bytes at 0x0010 changed, and they point at the counter now. Nothing\n");
    printf("wrote to the device between the reads; it answered differently\n");
    printf("because it is a device. That is what a hardware register does, and\n");
    printf("it is why the emulator has to send reads and writes through the bus\n");
    printf("instead of touching an array.\n");
    return 0;
}
