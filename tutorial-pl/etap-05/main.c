/*
 * Stage 5 — what a processor is.
 *
 * Every stage before this one ended at the panel: colour on glass. This one
 * leaves the board in its box and asks the question the rest of the guide rests
 * on: what is it that actually runs a program?
 *
 * The answer is smaller than most people expect. A processor is
 *
 *   - a few registers: slots that hold one number each, and that the processor
 *     reads and writes while it works,
 *   - a program counter: the number of the instruction to run next,
 *   - a stack: a place to put a number away for a while, which is how a jump
 *     into a subroutine finds its way back,
 *   - a loop that takes the instruction the program counter points at, does what
 *     it says, and comes back for the next one.
 *
 * That loop is the whole trick. It is written here for your computer instead of
 * the board, because a terminal shows every step of it and a panel shows only
 * the result:
 *
 *     cc -Wall -Wextra main.c -o etap05
 *     ./etap05
 *
 * The processor being imitated is the 6502, the one inside the console this
 * guide ends up emulating. What follows is its shape, not the chip: twelve
 * instructions out of the real one hundred and fifty-one, and only the two
 * simplest ways of naming the data they work on. Memory arrives in stage 06;
 * until then the instructions work on numbers the program carries with it.
 */
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ instructions
 * An instruction is one byte saying what to do, followed by zero, one or two
 * bytes of data. The first byte is the opcode. The names are the real 6502
 * mnemonics — LDA is "load into A", ADC is "add with carry" — and each one
 * appears here as a number, because a number is all the processor ever sees.
 */
#define OP_LDA_IMM  0x01   /* A = the byte that follows                    */
#define OP_LDX_IMM  0x02   /* X = the byte that follows                    */
#define OP_ADC_IMM  0x03   /* A = A + byte + carry                         */
#define OP_INX      0x04   /* X = X + 1                                    */
#define OP_DEX      0x05   /* X = X - 1                                    */
#define OP_JMP_ABS  0x06   /* pc = the two bytes that follow               */
#define OP_JSR_ABS  0x07   /* the same, but keep the way back first        */
#define OP_RTS      0x08   /* pc = the address taken off the stack         */
#define OP_BEQ_ABS  0x09   /* jump if the last result was zero             */
#define OP_BNE_ABS  0x0A   /* jump if it was not zero                      */
#define OP_NOP      0x0B   /* spend one instruction doing nothing          */
#define OP_HLT      0x0C   /* stop; the real chip has no such instruction  */

/* Only the trace needs these names, but they sit directly under the numbers so
 * that adding an instruction and forgetting its name is hard to do. The machine
 * has no names: this table is the first hint of what an assembler is for. */
static const char *opcode_name(uint8_t opcode)
{
    static const char *const names[] = {
        "???", "LDA", "LDX", "ADC", "INX", "DEX",
        "JMP", "JSR", "RTS", "BEQ", "BNE", "NOP", "HLT",
    };
    if (opcode < sizeof names / sizeof names[0]) {
        return names[opcode];
    }
    return "???";
}

/* ----------------------------------------------------------------- the processor
 * Registers are the processor's own slots and there are very few of them, for a
 * reason that matters once speed does: a processor that had to reach into memory
 * for every number would spend its life waiting. A holds what arithmetic works
 * on, X counts. The real 6502 has a third one, Y, which serves as a second
 * counter while walking through memory. Nothing here walks through memory yet,
 * so Y waits outside until there is something for it to do.
 */
#define STACK_SIZE 256
#define STACK_TOP  (STACK_SIZE - 1)

typedef struct {
    uint8_t  a;                  /* the accumulator: results land here      */
    uint8_t  x;                  /* the index register: our counter         */
    uint16_t pc;                 /* program counter: address of the next byte */
    uint8_t  sp;                 /* stack pointer: number of the free slot  */
    int      zero;               /* the last result was zero                */
    int      carry;              /* the last addition did not fit in a byte */
    uint8_t  stack[STACK_SIZE];  /* the stack itself                        */
    int      halted;             /* 1 once the program has stopped          */
    int      steps;              /* how many instructions ran               */
} cpu_t;

static cpu_t cpu;

/* ------------------------------------------------------------------- fetching
 * The program is a row of bytes and the program counter is a position in it, so
 * fetching is a read at that position followed by a step to the next one. The
 * check is what a jump to a mistyped address hits first; without it the program
 * would read whatever happens to lie behind the array.
 */
static uint8_t fetch(const uint8_t *program, int length)
{
    if (cpu.pc >= length) {
        printf("!! the program counter (%u) ran past the end of the program\n",
               (unsigned)cpu.pc);
        cpu.halted = 1;
        return 0;
    }
    return program[cpu.pc++];
}

/* A two-byte address, low byte first. The two fetches stand in separate
 * statements on purpose: inside one expression the language does not say which
 * call happens first, and that order decides whether the address comes out
 * right. Low byte first is the habit the real chip has. */
static uint16_t fetch_address(const uint8_t *program, int length)
{
    uint8_t low  = fetch(program, length);
    uint8_t high = fetch(program, length);
    return (uint16_t)(low | (high << 8));
}

/* ------------------------------------------------------------------ the stack
 * A stack is a pile of bytes with one rule: you add to the top and take from the
 * top, never from the middle. That is all a processor needs to remember where it
 * came from. `sp` is the number of the free slot, and the pile grows downwards,
 * so a push writes where sp points and then moves sp one slot down; a pull does
 * the opposite. On the real chip these 256 bytes are not an array of their own:
 * they sit in the main memory at 0x0100 to 0x01FF, which is where the stack
 * pointer's name comes from.
 */
static void push(uint8_t value)
{
    cpu.stack[cpu.sp--] = value;
}

static uint8_t pull(void)
{
    return cpu.stack[++cpu.sp];
}

/* --------------------------------------------------------------- arithmetic
 * The sum is computed in a wider type only to see whether it needed a ninth bit.
 * The register keeps the bottom eight, and the carry flag keeps the rest, which
 * is how a processor adds numbers bigger than one byte: the next addition starts
 * where this one stopped. Our numbers are small, so the carry stays zero, but
 * the flag has to be kept anyway — the conditional jump reads flags, and a flag
 * that is only sometimes right is worse than no flag.
 */
static void adc(uint8_t value)
{
    unsigned sum = (unsigned)cpu.a + value + (unsigned)cpu.carry;

    cpu.carry = (sum > 0xFF);
    cpu.a = (uint8_t)sum;
    cpu.zero = (cpu.a == 0);
}

/* ------------------------------------------------------------- one instruction
 * This switch is the processor. Everything else in the file either feeds it or
 * watches what it did. Note where the program counter is by the time a jump
 * writes into it: fetch has already moved it past the operand, so pc holds the
 * address of the next instruction — exactly the number JSR must put on the stack.
 */
static void step(const uint8_t *program, int length)
{
    uint16_t here = cpu.pc;              /* where this instruction started */
    uint8_t opcode = fetch(program, length);

    switch (opcode) {
    case OP_LDA_IMM:
        cpu.a = fetch(program, length);
        cpu.zero = (cpu.a == 0);
        break;
    case OP_LDX_IMM:
        cpu.x = fetch(program, length);
        cpu.zero = (cpu.x == 0);
        break;
    case OP_ADC_IMM:
        adc(fetch(program, length));
        break;
    case OP_INX:
        cpu.x++;                         /* one byte, so 255 + 1 wraps to 0 */
        cpu.zero = (cpu.x == 0);
        break;
    case OP_DEX:
        cpu.x--;
        cpu.zero = (cpu.x == 0);
        break;
    case OP_JMP_ABS:
        cpu.pc = fetch_address(program, length);
        break;
    case OP_JSR_ABS: {
        uint16_t target = fetch_address(program, length);
        push((uint8_t)(cpu.pc >> 8));    /* high byte first, */
        push((uint8_t)(cpu.pc & 0xFF));  /* so a pull reads low first */
        cpu.pc = target;
        break;
    }
    case OP_RTS: {
        uint8_t low  = pull();
        uint8_t high = pull();
        cpu.pc = (uint16_t)(low | (high << 8));
        break;
    }
    case OP_BEQ_ABS: {
        uint16_t target = fetch_address(program, length);
        if (cpu.zero) {
            cpu.pc = target;
        }
        break;
    }
    case OP_BNE_ABS: {
        uint16_t target = fetch_address(program, length);
        if (!cpu.zero) {
            cpu.pc = target;
        }
        break;
    }
    case OP_NOP:
        break;
    case OP_HLT:
        cpu.halted = 1;
        break;
    default:
        /* A real processor does something for all 256 numbers; a machine that
         * guesses here would hide a mistake instead of showing it. */
        printf("!! unknown opcode 0x%02X at address %u\n",
               (unsigned)opcode, (unsigned)here);
        cpu.halted = 1;
        break;
    }
}

/* ---------------------------------------------------------------- running it
 * The loop that turns all of the above into a processor: look at the program
 * counter, run the instruction there, come back. The two limits exist because
 * this is a teaching machine. A program with a jump in the wrong place would
 * otherwise look like a terminal that hung, not like a mistake in the program.
 */
#define STEP_LIMIT 200

static void reset(void)
{
    cpu.a = 0;
    cpu.x = 0;
    cpu.pc = 0;
    cpu.sp = STACK_TOP;                  /* the last slot; the pile grows down */
    cpu.zero = 0;
    cpu.carry = 0;
    cpu.halted = 0;
    cpu.steps = 0;
    /* The stack bytes are left alone. A push overwrites the slot it needs, and
     * sp is one byte wide, so it wraps around the 256 slots instead of running
     * off them — the same wrap the real chip's stack pointer has. */
}

static void run(const uint8_t *program, int length, uint8_t expected_a)
{
    reset();

    while (!cpu.halted && cpu.pc < length && cpu.steps < STEP_LIMIT) {
        /* Print the instruction next to the state it is about to run in, so
         * every line shows the effect of the line above it. */
        printf("%04X  %-4s  A=%3u X=%3u  Z=%d C=%d  SP=%3u\n",
               (unsigned)cpu.pc, opcode_name(program[cpu.pc]),
               (unsigned)cpu.a, (unsigned)cpu.x,
               cpu.zero, cpu.carry, (unsigned)cpu.sp);
        step(program, length);
        cpu.steps++;
    }

    if (!cpu.halted) {
        /* Two ways out with the processor still running, and they mean different
         * things: either the program never stops, or it ended without a HLT. */
        if (cpu.steps == STEP_LIMIT) {
            printf("!! %d instructions and still going: the program probably loops forever\n",
                   STEP_LIMIT);
        } else {
            printf("!! the program ran off the end (%d bytes) without a HLT\n", length);
        }
    }
    printf("%d instructions, A = %u, X = %u, SP = %u -- expected A = %u: %s\n",
           cpu.steps, (unsigned)cpu.a, (unsigned)cpu.x, (unsigned)cpu.sp,
           (unsigned)expected_a, cpu.a == expected_a ? "ok" : "wrong");
}

/* --------------------------------------------------------------- the programs
 * Three programs in the form the processor sees them: an opcode, then the bytes
 * belonging to it. The comment on the right is the assembly an assembler would
 * print, and the address on the left is a position in this very list — which is
 * why a jump stores an address, and why putting one instruction in the middle
 * moves every address below it.
 */

/* 7 x 6, by adding seven six times. X counts the additions down to zero, and the
 * zero flag that DEX leaves behind is what ends the loop. */
static const uint8_t program_multiply[] = {
    OP_LDA_IMM, 0,       /* 0000  LDA #0     A = 0, the result so far     */
    OP_LDX_IMM, 6,       /* 0002  LDX #6     X = 6, additions still to do */
    OP_ADC_IMM, 7,       /* 0004  ADC #7     A = A + 7                    */
    OP_DEX,              /* 0006  DEX        X = X - 1                    */
    OP_BNE_ABS, 4, 0,    /* 0007  BNE 0004   not zero yet? add again      */
    OP_HLT,              /* 000A  HLT                                     */
};

/* The addition sits somewhere else in the program this time, and the way back is
 * a number on the stack instead of an address written into the code. That is what
 * makes a subroutine a subroutine: the piece doing the work does not need to know
 * who called it or from where. */
static const uint8_t program_call[] = {
    OP_LDA_IMM, 5,       /* 0000  LDA #5     A = 5                        */
    OP_JSR_ABS, 6, 0,    /* 0002  JSR 0006   push 0005, jump to 0006      */
    OP_HLT,              /* 0005  HLT        RTS comes back here          */
    OP_ADC_IMM, 5,       /* 0006  ADC #5     A = A + 5                    */
    OP_RTS,              /* 0008  RTS        pull 0005, jump there        */
};

/* An unconditional jump, and the instruction it jumps over. Nothing can reach
 * the LDA at 0005, so A never becomes 9; if the test below ever says "wrong",
 * the jump went to the wrong address. */
static const uint8_t program_skip[] = {
    OP_LDA_IMM, 1,       /* 0000  LDA #1     A = 1                        */
    OP_JMP_ABS, 7, 0,    /* 0002  JMP 0007   over the next instruction    */
    OP_LDA_IMM, 9,       /* 0005  LDA #9     never runs                   */
    OP_HLT,              /* 0007  HLT                                     */
};

int main(void)
{
    /* sizeof gives the number of bytes in the array: instructions and their data
     * together. run() needs it to notice a program counter that ran off the end. */
    printf("1. 7 x 6, by adding seven six times\n");
    run(program_multiply, sizeof program_multiply, 42);

    printf("\n2. a subroutine: the way back is on the stack\n");
    run(program_call, sizeof program_call, 10);

    printf("\n3. an unconditional jump: what it skips never runs\n");
    run(program_skip, sizeof program_skip, 1);

    return 0;
}
