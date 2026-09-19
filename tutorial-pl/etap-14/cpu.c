/*
 * cpu.c — the same fetch-and-do loop as stage 13, plus the interrupt.
 *
 * The loop is unchanged: read the byte the program counter points at, that
 * byte picks one small job out of the switch below, do the job, move on. What
 * is new is the line at the top of cpu_run: before each instruction, ask the
 * machine whether the cartridge is pulling the interrupt wire. If it is, and
 * if the processor's own I flag lets it through, the processor puts down what
 * it was doing and jumps to the address the cartridge keeps for that signal.
 *
 * Everything here is one case per instruction, so the game in main.c can be
 * read against it line by line.
 */
#include "cpu.h"

cpu_t cpu;

#define STACK_PAGE 0x0100

static void push(uint8_t value)
{
    machine_write((uint16_t)(STACK_PAGE + cpu.sp), value);
    cpu.sp--;
}

static uint8_t pull(void)
{
    cpu.sp++;
    return machine_read((uint16_t)(STACK_PAGE + cpu.sp));
}

static void set_flags(uint8_t value)
{
    cpu.status &= (uint8_t)~(FLAG_Z | FLAG_N);
    if (value == 0)   cpu.status |= FLAG_Z;
    if (value & 0x80) cpu.status |= FLAG_N;
}

static uint8_t immediate(void)
{
    return machine_read(cpu.pc++);
}

static uint16_t zero_page(void)
{
    return machine_read(cpu.pc++);
}

static uint16_t absolute(void)              /* two bytes, the low one first */
{
    uint8_t low = machine_read(cpu.pc++);
    uint8_t high = machine_read(cpu.pc++);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static void compare(uint8_t left, uint8_t right)
{
    /* A comparison is a subtraction the processor throws away: only the flags
     * survive, and the branch that follows reads them. */
    cpu.status &= (uint8_t)~(FLAG_C | FLAG_Z | FLAG_N);
    if (left >= right) cpu.status |= FLAG_C;
    set_flags((uint8_t)(left - right));
}

static void add_with_carry(uint8_t value)
{
    int carry = (cpu.status & FLAG_C) ? 1 : 0;
    int sum = cpu.a + value + carry;

    cpu.status &= (uint8_t)~FLAG_C;
    if (sum > 0xFF) cpu.status |= FLAG_C;
    cpu.a = (uint8_t)sum;
    set_flags(cpu.a);
}

static void branch(int take)
{
    /* A branch carries a distance, not a place: it is counted from the byte
     * after the branch itself, which is where the program counter already is. */
    int8_t distance = (int8_t)machine_read(cpu.pc++);
    if (take) cpu.pc = (uint16_t)(cpu.pc + distance);
}

/* --------------------------------------------------------------- interrupt
 * The moment the wire is pulled: keep the way back, keep the flags, refuse
 * further signals, and go where the cartridge says. RTI undoes all four.
 */
static void interrupt(void)
{
    push((uint8_t)(cpu.pc >> 8));
    push((uint8_t)(cpu.pc & 0xFF));
    push((uint8_t)((cpu.status & (uint8_t)~FLAG_B) | FLAG_U));
    cpu.status |= FLAG_I;
    cpu.pc = (uint16_t)(machine_read(0xFFFE)
                        | ((uint16_t)machine_read(0xFFFF) << 8));
    machine_irq_taken();
}

void cpu_reset(void)
{
    cpu.a = cpu.x = cpu.y = 0;
    cpu.sp = 0xFD;
    cpu.status = FLAG_I | FLAG_U;   /* interrupts stay off until the program says so */
    cpu.instructions = 0;
    cpu.stopped = 0;
    cpu.stop_opcode = 0;
    /* The reset vector sits at the top of the address space, which is why the
     * last bank of the cartridge is the one wired to the top of the map. */
    cpu.pc = (uint16_t)(machine_read(0xFFFC)
                        | ((uint16_t)machine_read(0xFFFD) << 8));
}

static void step(void)
{
    uint8_t opcode = machine_read(cpu.pc++);

    switch (opcode) {
    /* ---- flags and small moves ---- */
    case 0x78: cpu.status |= FLAG_I; break;                     /* SEI */
    case 0x58: cpu.status &= (uint8_t)~FLAG_I; break;           /* CLI */
    case 0xD8: cpu.status &= (uint8_t)~FLAG_D; break;           /* CLD */
    case 0x18: cpu.status &= (uint8_t)~FLAG_C; break;           /* CLC */
    case 0xEA: break;                                           /* NOP */
    case 0xAA: cpu.x = cpu.a; set_flags(cpu.x); break;          /* TAX */
    case 0x8A: cpu.a = cpu.x; set_flags(cpu.a); break;          /* TXA */
    case 0x9A: cpu.sp = cpu.x; break;                           /* TXS */
    case 0xE8: cpu.x++; set_flags(cpu.x); break;                /* INX */
    case 0xCA: cpu.x--; set_flags(cpu.x); break;                /* DEX */
    case 0xC8: cpu.y++; set_flags(cpu.y); break;                /* INY */
    case 0x88: cpu.y--; set_flags(cpu.y); break;                /* DEY */
    case 0x48: push(cpu.a); break;                              /* PHA */
    case 0x68: cpu.a = pull(); set_flags(cpu.a); break;         /* PLA */
    case 0x4A:                                                  /* LSR A */
        cpu.status = (uint8_t)((cpu.status & (uint8_t)~FLAG_C)
                               | (cpu.a & 1));
        cpu.a = (uint8_t)(cpu.a >> 1);
        set_flags(cpu.a);
        break;

    /* ---- loading ---- */
    case 0xA9: cpu.a = immediate(); set_flags(cpu.a); break;    /* LDA # */
    case 0xA5: cpu.a = machine_read(zero_page()); set_flags(cpu.a); break;
    case 0xAD: cpu.a = machine_read(absolute()); set_flags(cpu.a); break;
    case 0xBD:                                                  /* LDA abs,X */
        cpu.a = machine_read((uint16_t)(absolute() + cpu.x));
        set_flags(cpu.a);
        break;
    case 0xA2: cpu.x = immediate(); set_flags(cpu.x); break;    /* LDX # */
    case 0xA0: cpu.y = immediate(); set_flags(cpu.y); break;    /* LDY # */

    /* ---- storing ---- */
    case 0x85: machine_write(zero_page(), cpu.a); break;        /* STA zp */
    case 0x8D: machine_write(absolute(), cpu.a); break;         /* STA abs */
    case 0x9D:                                                  /* STA abs,X */
        machine_write((uint16_t)(absolute() + cpu.x), cpu.a);
        break;

    /* ---- counting ---- */
    case 0xE6: {                                                /* INC zp */
        uint16_t at = zero_page();
        uint8_t value = (uint8_t)(machine_read(at) + 1);
        machine_write(at, value);
        set_flags(value);
        break;
    }
    case 0xEE: {                                                /* INC abs */
        uint16_t at = absolute();
        uint8_t value = (uint8_t)(machine_read(at) + 1);
        machine_write(at, value);
        set_flags(value);
        break;
    }

    /* ---- arithmetic and logic ---- */
    case 0x69: add_with_carry(immediate()); break;              /* ADC # */
    case 0x29: cpu.a &= immediate(); set_flags(cpu.a); break;   /* AND # */
    case 0x09: cpu.a |= immediate(); set_flags(cpu.a); break;   /* ORA # */
    case 0xC9: compare(cpu.a, immediate()); break;              /* CMP # */
    case 0xE0: compare(cpu.x, immediate()); break;              /* CPX # */
    case 0xC0: compare(cpu.y, immediate()); break;              /* CPY # */

    /* ---- branches ---- */
    case 0xD0: branch(!(cpu.status & FLAG_Z)); break;           /* BNE */
    case 0xF0: branch(cpu.status & FLAG_Z); break;              /* BEQ */
    case 0x10: branch(!(cpu.status & FLAG_N)); break;           /* BPL */
    case 0x30: branch(cpu.status & FLAG_N); break;              /* BMI */

    /* ---- going elsewhere ---- */
    case 0x4C: cpu.pc = absolute(); break;                      /* JMP */
    case 0x20: {                                                /* JSR */
        uint16_t target = absolute();
        push((uint8_t)((cpu.pc - 1) >> 8));
        push((uint8_t)((cpu.pc - 1) & 0xFF));
        cpu.pc = target;
        break;
    }
    case 0x60: {                                                /* RTS */
        uint8_t low = pull();
        uint8_t high = pull();
        cpu.pc = (uint16_t)((((uint16_t)high << 8) | low) + 1);
        break;
    }
    case 0x40: {                                                /* RTI */
        cpu.status = (uint8_t)((pull() & (uint8_t)~FLAG_B) | FLAG_U);
        uint8_t low = pull();
        uint8_t high = pull();
        cpu.pc = (uint16_t)(((uint16_t)high << 8) | low);
        break;
    }

    default:
        cpu.stopped = 1;
        cpu.stop_opcode = opcode;
        break;
    }
}

void cpu_run(int budget)
{
    for (int n = 0; n < budget && !cpu.stopped; n++) {
        /* The wire is checked between two instructions and never inside one.
         * A signal that arrives while the I flag is set waits, which is how a
         * program protects a piece of work it cannot have interrupted. */
        if (machine_irq() && !(cpu.status & FLAG_I)) interrupt();
        step();
        cpu.instructions++;
    }
}
