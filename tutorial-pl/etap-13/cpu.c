/*
 * cpu.c — one instruction at a time.
 *
 * The shape of this file is the shape of every emulator's processor core:
 * read the byte the program counter points at, that byte is a number that
 * picks one of a couple of hundred small jobs, do the job, and move the
 * program counter past the bytes the job used.
 *
 * Two ideas from stage 06 are visible here without any explanation needed:
 *
 *   - an operand can be an immediate value (`lda #$20`), a place in memory
 *     (`sta $2007`), or an address computed from an index register
 *     (`lda COLOURS,x`). Each of those has its own small helper below,
 *     and the helper's only job is to produce the address.
 *   - a branch is not a jump to a fixed place: the instruction carries a
 *     distance, counted from the byte after the branch itself. That is why
 *     the assembler could not fill the byte in until it knew where the
 *     label ended up.
 *
 * The game in this stage only needs part of the instruction set, so only
 * that part is here. Anything missing is a case that returns false, and the
 * host reports which instruction stopped the processor instead of silently
 * running garbage.
 */
#include "cpu.h"

cpu_t cpu;

/* where and why the processor stopped, for the host to print */
uint16_t stopped_at;
uint8_t  stopped_opcode;

static void set_flags(uint8_t value)
{
    cpu.status &= (uint8_t)~(FLAG_Z | FLAG_N);
    if (value == 0)          cpu.status |= FLAG_Z;
    if (value & 0x80)        cpu.status |= FLAG_N;
}

static uint16_t read_operand(void)          /* two bytes, low first */
{
    uint8_t low = bus_read(cpu.pc++);
    uint8_t high = bus_read(cpu.pc++);
    return (uint16_t)(low | (high << 8));
}

void cpu_reset(void)
{
    cpu.a = cpu.x = cpu.y = 0;
    cpu.sp = 0xFD;
    cpu.status = FLAG_I | FLAG_U;
    cpu.instructions = 0;
    /* The reset vector sits at the top of the address space, and the
     * cartridge has to have a bank there — that is the bank that can never
     * be switched out. */
    cpu.pc = (uint16_t)(bus_read(0xFFFC) | (bus_read(0xFFFD) << 8));
}

bool cpu_step(void)
{
    uint8_t  op  = bus_read(cpu.pc++);
    uint16_t adr;
    uint8_t  value;

    cpu.instructions++;

    switch (op) {
    case 0x00:                                  /* brk: stop for good */
        return false;

    /* ---- load and store ------------------------------------------- */
    case 0xA9: cpu.a = bus_read(cpu.pc++); set_flags(cpu.a); break;
    case 0xA5: cpu.a = bus_read(bus_read(cpu.pc++)); set_flags(cpu.a); break;
    case 0xAD: cpu.a = bus_read(read_operand()); set_flags(cpu.a); break;
    case 0xBD:                                  /* lda absolute,x */
        adr = (uint16_t)(read_operand() + cpu.x);
        cpu.a = bus_read(adr);
        set_flags(cpu.a);
        break;
    case 0xB1: {                                /* lda (pointer),y */
        uint8_t zero = bus_read(cpu.pc++);
        uint16_t base = (uint16_t)(bus_read(zero)
                                   | (bus_read((uint8_t)(zero + 1)) << 8));
        cpu.a = bus_read((uint16_t)(base + cpu.y));
        set_flags(cpu.a);
        break;
    }
    case 0xA2: cpu.x = bus_read(cpu.pc++); set_flags(cpu.x); break;
    case 0xA6: cpu.x = bus_read(bus_read(cpu.pc++)); set_flags(cpu.x); break;
    case 0xAE: cpu.x = bus_read(read_operand()); set_flags(cpu.x); break;
    case 0xA0: cpu.y = bus_read(cpu.pc++); set_flags(cpu.y); break;
    case 0xA4: cpu.y = bus_read(bus_read(cpu.pc++)); set_flags(cpu.y); break;
    case 0x85: bus_write(bus_read(cpu.pc++), cpu.a); break;
    case 0x8D: bus_write(read_operand(), cpu.a); break;
    case 0x91: {                                /* sta (pointer),y */
        uint8_t zero = bus_read(cpu.pc++);
        uint16_t base = (uint16_t)(bus_read(zero)
                                   | (bus_read((uint8_t)(zero + 1)) << 8));
        bus_write((uint16_t)(base + cpu.y), cpu.a);
        break;
    }

    /* ---- move a byte between registers ---------------------------- */
    case 0xAA: cpu.x = cpu.a; set_flags(cpu.x); break;
    case 0x8A: cpu.a = cpu.x; set_flags(cpu.a); break;
    case 0xA8: cpu.y = cpu.a; set_flags(cpu.y); break;
    case 0x98: cpu.a = cpu.y; set_flags(cpu.a); break;
    case 0x9A: cpu.sp = cpu.x; break;           /* txs: no flags */

    /* ---- count ---------------------------------------------------- */
    case 0xE8: cpu.x++; set_flags(cpu.x); break;
    case 0xC8: cpu.y++; set_flags(cpu.y); break;
    case 0xCA: cpu.x--; set_flags(cpu.x); break;
    case 0x88: cpu.y--; set_flags(cpu.y); break;
    case 0xE6: {                                /* inc in page zero */
        uint8_t zero = bus_read(cpu.pc++);
        value = (uint8_t)(bus_read(zero) + 1);
        bus_write(zero, value);
        set_flags(value);
        break;
    }
    case 0xEE: {                                /* inc anywhere */
        adr = read_operand();
        value = (uint8_t)(bus_read(adr) + 1);
        bus_write(adr, value);
        set_flags(value);
        break;
    }

    /* ---- arithmetic ----------------------------------------------- */
    case 0x65: case 0x69: {                     /* adc, from memory or a literal */
        uint8_t carry = (uint8_t)(cpu.status & FLAG_C);
        uint16_t operand = (op == 0x65) ? bus_read(bus_read(cpu.pc++))
                                        : bus_read(cpu.pc++);
        uint16_t sum = (uint16_t)(cpu.a + operand + carry);
        cpu.status &= (uint8_t)~(FLAG_C | FLAG_V);
        if (sum > 0xFF) cpu.status |= FLAG_C;
        /* overflow is about signed numbers: both inputs the same sign and
         * the result the other sign */
        if (~(cpu.a ^ (uint8_t)sum) & (cpu.a ^ (uint8_t)(sum >> 8)) & 0x80)
            cpu.status |= FLAG_V;
        cpu.a = (uint8_t)sum;
        set_flags(cpu.a);
        break;
    }
    case 0x25: cpu.a &= bus_read(bus_read(cpu.pc++)); set_flags(cpu.a); break;
    case 0x29: cpu.a &= bus_read(cpu.pc++); set_flags(cpu.a); break;
    case 0x49: cpu.a ^= bus_read(cpu.pc++); set_flags(cpu.a); break;

    /* ---- compare: a subtraction that throws the result away ------- */
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

    /* ---- branches: a signed distance from the next instruction ---- */
    case 0x10: case 0x30: case 0xD0: case 0xF0: {
        int8_t distance = (int8_t)bus_read(cpu.pc++);
        bool take;
        if      (op == 0x10) take = !(cpu.status & FLAG_N);
        else if (op == 0x30) take =  (cpu.status & FLAG_N) != 0;
        else if (op == 0xD0) take = !(cpu.status & FLAG_Z);
        else                 take =  (cpu.status & FLAG_Z) != 0;
        if (take)
            cpu.pc = (uint16_t)(cpu.pc + distance);
        break;
    }

    /* ---- go somewhere else, and come back ------------------------- */
    case 0x4C: cpu.pc = read_operand(); break;
    case 0x20: {                                /* jsr: remember where we were */
        uint16_t target = read_operand();
        uint16_t back = (uint16_t)(cpu.pc - 1);
        bus_write((uint16_t)(0x0100 + cpu.sp--), (uint8_t)(back >> 8));
        bus_write((uint16_t)(0x0100 + cpu.sp--), (uint8_t)(back & 0xFF));
        cpu.pc = target;
        break;
    }
    case 0x60: {                                /* rts: and go back */
        uint8_t low  = bus_read((uint16_t)(0x0100 + ++cpu.sp));
        uint8_t high = bus_read((uint16_t)(0x0100 + ++cpu.sp));
        cpu.pc = (uint16_t)((low | (high << 8)) + 1);
        break;
    }

    /* ---- flags ---------------------------------------------------- */
    case 0x18: cpu.status &= (uint8_t)~FLAG_C; break;
    case 0xD8: cpu.status &= (uint8_t)~FLAG_D; break;
    case 0x58: cpu.status &= (uint8_t)~FLAG_I; break;   /* cli */
    case 0x78: cpu.status |= FLAG_I; break;             /* sei */
    case 0x24: case 0x2C: {                     /* bit: look without taking */
        adr = (op == 0x24) ? bus_read(cpu.pc++) : read_operand();
        value = bus_read(adr);
        cpu.status &= (uint8_t)~(FLAG_Z | FLAG_N);
        if ((cpu.a & value) == 0) cpu.status |= FLAG_Z;
        /* N comes from bit *seven* of the byte, V from bit six. Reading the
         * wrong bit here is the kind of mistake that costs an afternoon:
         * everything else looks right, and the one loop in the program that
         * waits for a frame never sees one. */
        cpu.status |= (uint8_t)(value & FLAG_N);
        break;
    }
    case 0xEA: break;                           /* nop */

    default:
        /* The program has run into an instruction this core does not know.
         * Stopping and saying which one is the only useful thing to do: the
         * alternative is to keep going and run nonsense. */
        stopped_at = (uint16_t)(cpu.pc - 1);
        stopped_opcode = op;
        return false;
    }
    return true;
}
