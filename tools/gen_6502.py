#!/usr/bin/env python3
"""
gen_6502.py — generates the 6502 instruction dispatch (src/cpu_ops.h).

The opcode matrix lives here as the single source of truth: mnemonic ->
{mode: opcode}. The script emits one C `case` per opcode, with the right
addressing macro and the right base cycle count, so the hand-written C
core (cpu6502.c) only contains the macro toolbox and the memory
interface.

The emitted cases are written in the *macros* of cpu6502.c — RD()/WR()/
FETCH8(), A_ORA()/A_ADC()/..., IMM()/ZP()/ABX()/..., CYCLES(), PUSH() —
which are defined over cpu_run()'s automatic variables
(pc/a/x/y/sp/p/cyc/page). They are macros and not functions because a
function cannot touch its caller's locals, and the whole point of the
locals is that the compiler can keep them in host registers across the
bus accesses; a file-scope static cache cannot.

Every operand here appears exactly once in the emitted expression, in
the same place and order the old function-call form evaluated it, so a
macro like INX() (which steps pc and reads memory) is never evaluated
twice. Where a case needs a statement — a branch, JSR, JMP (ind) — it
calls a statement macro (BRANCH_TAKE/JMP_IND) rather than duplicating a
side-effecting operand.

Run:  python3 tools/gen_6502.py > src/cpu_ops.h
      python3 tools/gen_6502.py --check      (verify against cpu_ops.h)
"""
import sys

# mode -> (addressing macro, extra cycles)
MODES = {
    "imp": ("IMP", 0),
    "acc": ("ACC", 0),
    "imm": ("IMM()", 0),
    "zp":  ("ZP()", 0),
    "zpx": ("ZPX()", 0),
    "zpy": ("ZPY()", 0),
    "abs": ("ABS_()", 0),
    "abx": ("ABX()", 0),     # may add a page-cross cycle
    "aby": ("ABY()", 0),
    "inx": ("INX()", 0),
    "iny": ("INY()", 0),
    "ind": ("IND()", 0),     # unused: JMP (ind) goes through JMP_IND()
    "rel": ("REL()", 0),     # unused: branches go through BRANCH_TAKE()
}

# ---------------------------------------------------------------- opcodes
# mnemonic -> {mode: opcode}
OPS = {
    "ADC": {"imm": 0x69, "zp": 0x65, "zpx": 0x75, "abs": 0x6D, "abx": 0x7D,
            "aby": 0x79, "inx": 0x61, "iny": 0x71},
    "AND": {"imm": 0x29, "zp": 0x25, "zpx": 0x35, "abs": 0x2D, "abx": 0x3D,
            "aby": 0x39, "inx": 0x21, "iny": 0x31},
    "ASL": {"acc": 0x0A, "zp": 0x06, "zpx": 0x16, "abs": 0x0E, "abx": 0x1E},
    "BIT": {"zp": 0x24, "abs": 0x2C},
    "BPL": {"rel": 0x10}, "BMI": {"rel": 0x30},
    "BVC": {"rel": 0x50}, "BVS": {"rel": 0x70},
    "BCC": {"rel": 0x90}, "BCS": {"rel": 0xB0},
    "BNE": {"rel": 0xD0}, "BEQ": {"rel": 0xF0},
    "BRK": {"imp": 0x00},
    "CLC": {"imp": 0x18}, "SEC": {"imp": 0x38},
    "CLI": {"imp": 0x58}, "SEI": {"imp": 0x78},
    "CLV": {"imp": 0xB8}, "CLD": {"imp": 0xD8}, "SED": {"imp": 0xF8},
    "CMP": {"imm": 0xC9, "zp": 0xC5, "zpx": 0xD5, "abs": 0xCD, "abx": 0xDD,
            "aby": 0xD9, "inx": 0xC1, "iny": 0xD1},
    "CPX": {"imm": 0xE0, "zp": 0xE4, "abs": 0xEC},
    "CPY": {"imm": 0xC0, "zp": 0xC4, "abs": 0xCC},
    "DEC": {"zp": 0xC6, "zpx": 0xD6, "abs": 0xCE, "abx": 0xDE},
    "DEX": {"imp": 0xCA}, "DEY": {"imp": 0x88},
    "EOR": {"imm": 0x49, "zp": 0x45, "zpx": 0x55, "abs": 0x4D, "abx": 0x5D,
            "aby": 0x59, "inx": 0x41, "iny": 0x51},
    "INC": {"zp": 0xE6, "zpx": 0xF6, "abs": 0xEE, "abx": 0xFE},
    "INX": {"imp": 0xE8}, "INY": {"imp": 0xC8},
    "JMP": {"abs": 0x4C, "ind": 0x6C},
    "JSR": {"abs": 0x20},
    "LDA": {"imm": 0xA9, "zp": 0xA5, "zpx": 0xB5, "abs": 0xAD, "abx": 0xBD,
            "aby": 0xB9, "inx": 0xA1, "iny": 0xB1},
    "LDX": {"imm": 0xA2, "zp": 0xA6, "zpy": 0xB6, "abs": 0xAE, "aby": 0xBE},
    "LDY": {"imm": 0xA0, "zp": 0xA4, "zpx": 0xB4, "abs": 0xAC, "abx": 0xBC},
    "LSR": {"acc": 0x4A, "zp": 0x46, "zpx": 0x56, "abs": 0x4E, "abx": 0x5E},
    "NOP": {"imp": 0xEA},
    "ORA": {"imm": 0x09, "zp": 0x05, "zpx": 0x15, "abs": 0x0D, "abx": 0x1D,
            "aby": 0x19, "inx": 0x01, "iny": 0x11},
    "PHA": {"imp": 0x48}, "PHP": {"imp": 0x08},
    "PLA": {"imp": 0x68}, "PLP": {"imp": 0x28},
    "ROL": {"acc": 0x2A, "zp": 0x26, "zpx": 0x36, "abs": 0x2E, "abx": 0x3E},
    "ROR": {"acc": 0x6A, "zp": 0x66, "zpx": 0x76, "abs": 0x6E, "abx": 0x7E},
    "RTI": {"imp": 0x40}, "RTS": {"imp": 0x60},
    "SBC": {"imm": 0xE9, "zp": 0xE5, "zpx": 0xF5, "abs": 0xED, "abx": 0xFD,
            "aby": 0xF9, "inx": 0xE1, "iny": 0xF1},
    "STA": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D, "abx": 0x9D, "aby": 0x99,
            "inx": 0x81, "iny": 0x91},
    "STX": {"zp": 0x86, "zpy": 0x96, "abs": 0x8E},
    "STY": {"zp": 0x84, "zpx": 0x94, "abs": 0x8C},
    "TAX": {"imp": 0xAA}, "TAY": {"imp": 0xA8},
    "TSX": {"imp": 0xBA}, "TXA": {"imp": 0x8A},
    "TXS": {"imp": 0x9A}, "TYA": {"imp": 0x98},
    # common illegal opcodes that some real games use
    "LAX": {"zp": 0xA7, "abs": 0xAF, "iny": 0xB3, "zpy": 0xB7, "aby": 0xBF,
            "inx": 0xA3},
    "SAX": {"zp": 0x87, "zpy": 0x97, "abs": 0x8F, "inx": 0x83},
    "DCP": {"zp": 0xC7, "zpx": 0xD7, "abs": 0xCF, "abx": 0xDF, "aby": 0xDB,
            "iny": 0xD3, "inx": 0xC3},
    "ISC": {"zp": 0xE7, "zpx": 0xF7, "abs": 0xEF, "abx": 0xFF, "aby": 0xFB,
            "iny": 0xF3, "inx": 0xE3},
    "SLO": {"zp": 0x07, "zpx": 0x17, "abs": 0x0F, "abx": 0x1F, "aby": 0x1B,
            "iny": 0x13, "inx": 0x03},
    "RLA": {"zp": 0x27, "zpx": 0x37, "abs": 0x2F, "abx": 0x3F, "aby": 0x3B,
            "iny": 0x33, "inx": 0x23},
    "SRE": {"zp": 0x47, "zpx": 0x57, "abs": 0x4F, "abx": 0x5F, "aby": 0x5B,
            "iny": 0x53, "inx": 0x43},
    "RRA": {"zp": 0x67, "zpx": 0x77, "abs": 0x6F, "abx": 0x7F, "aby": 0x7B,
            "iny": 0x73, "inx": 0x63},
}

# base cycle counts: mnemonic -> mode -> cycles
CYCLES = {
    "ADC": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "AND": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "ASL": {"acc": 2, "zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "BIT": {"zp": 3, "abs": 4},
    "BRK": {"imp": 7},
    "CMP": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "CPX": {"imm": 2, "zp": 3, "abs": 4},
    "CPY": {"imm": 2, "zp": 3, "abs": 4},
    "DEC": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "EOR": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "INC": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "JMP": {"abs": 3, "ind": 5},
    "JSR": {"abs": 6},
    "LDA": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "LDX": {"imm": 2, "zp": 3, "zpy": 4, "abs": 4, "aby": 4},
    "LDY": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4},
    "LSR": {"acc": 2, "zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "ORA": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "ROL": {"acc": 2, "zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "ROR": {"acc": 2, "zp": 5, "zpx": 6, "abs": 6, "abx": 7},
    "SBC": {"imm": 2, "zp": 3, "zpx": 4, "abs": 4, "abx": 4, "aby": 4, "inx": 6, "iny": 5},
    "STA": {"zp": 3, "zpx": 4, "abs": 4, "abx": 5, "aby": 5, "inx": 6, "iny": 6},
    "STX": {"zp": 3, "zpy": 4, "abs": 4},
    "STY": {"zp": 3, "zpx": 4, "abs": 4},
    "LAX": {"zp": 3, "zpy": 4, "abs": 4, "aby": 4, "iny": 5, "inx": 6},
    "SAX": {"zp": 3, "zpy": 4, "abs": 4, "inx": 6},
    "DCP": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
    "ISC": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
    "SLO": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
    "RLA": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
    "SRE": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
    "RRA": {"zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7, "iny": 6, "inx": 6},
}
IMPLIED_2 = ("CLC SEC CLI SEI CLV CLD SED DEX DEY INX INY TAX TAY TSX TXA TXS TYA NOP")
IMPLIED_3 = ("PHA PHP")
IMPLIED_4 = ("PLA PLP")
IMPLIED_6 = ("RTS RTI")
BRANCHES = set("BPL BMI BVC BVS BCC BCS BNE BEQ".split())

READ_OPS = set("LDA LDX LDY EOR AND ORA ADC SBC CMP CPX CPY LAX".split())
NOP_ILLEGAL = None  # filled below

# branch mnemonic -> the flag test handed to BRANCH_TAKE()
BRANCH_COND = {
    "BPL": "!(p & F_N)", "BMI": "p & F_N",
    "BVC": "!(p & F_V)", "BVS": "p & F_V",
    "BCC": "!(p & F_C)", "BCS": "p & F_C",
    "BNE": "!(p & F_Z)", "BEQ": "p & F_Z",
}

# implied opcode -> body, written in the cpu6502.c macros
IMPLIED_BODY = {
    "CLC": "p &= (uint8_t)~F_C", "SEC": "p |= F_C",
    "CLI": "p &= (uint8_t)~F_I", "SEI": "p |= F_I",
    "CLV": "p &= (uint8_t)~F_V", "CLD": "p &= (uint8_t)~F_D",
    "SED": "p |= F_D",
    "DEX": "x--; SETZN(x)", "DEY": "y--; SETZN(y)",
    "INX": "x++; SETZN(x)", "INY": "y++; SETZN(y)",
    "TAX": "x = a; SETZN(x)",
    "TAY": "y = a; SETZN(y)",
    "TSX": "x = sp; SETZN(x)",
    "TXA": "a = x; SETZN(a)",
    "TXS": "sp = x",
    "TYA": "a = y; SETZN(a)",
    "PHA": "PUSH(a)", "PHP": "PUSH((uint8_t)(p | F_B | F_U))",
    "PLA": "a = PULL(); SETZN(a)",
    "PLP": "p = (uint8_t)((PULL() & (uint8_t)~F_B) | F_U)",
    "NOP": "",
    "BRK": "BRK()",
    "RTS": "RTS()", "RTI": "RTI()",
}

# read/ALU opcode -> macro applied to the operand
ALU_OP = {
    "EOR": "A_EOR", "AND": "A_AND", "ORA": "A_ORA",
    "ADC": "A_ADC", "SBC": "A_SBC",
    "CMP": "A_CMP", "CPX": "A_CPX", "CPY": "A_CPY",
}
LOAD_REG = {"LDA": "a", "LDX": "x", "LDY": "y"}
STORE_REG = {"STA": "a", "STX": "x", "STY": "y"}
SHIFT_FN = {"ASL": "ASL", "LSR": "LSR", "ROL": "ROL",
            "ROR": "ROR", "INC": "INC8", "DEC": "DEC8"}
RMW_ALU = {"DCP": ("DEC8", "A_CMP"), "ISC": ("INC8", "A_SBC"),
           "SLO": ("ASL", "A_ORA"), "RLA": ("ROL", "A_AND"),
           "SRE": ("LSR", "A_EOR"), "RRA": ("ROR", "A_ADC")}


def base_cycles(mn, mode):
    if mn in BRANCHES:
        return 2
    if mn in CYCLES:
        return CYCLES[mn][mode]
    if mn in IMPLIED_2.split() or mn == "NOP":
        return 2
    if mn in IMPLIED_3.split():
        return 3
    if mn in IMPLIED_4.split():
        return 4
    if mn in IMPLIED_6.split():
        return 6
    raise KeyError((mn, mode))


def emit_case(mn, mode, op, out):
    """out is a list.append-style callable."""
    a, _ = MODES[mode]
    cyc = base_cycles(mn, mode)
    # the modes whose address may cross a page add one cycle
    c = f"{cyc} + page" if mode in ("abx", "aby", "iny") else f"{cyc}"
    w = out

    # --- branches: BRANCH_TAKE evaluates the condition once and sets cyc
    if mn in BRANCHES:
        w(f"        case 0x{op:02X}: BRANCH_TAKE({BRANCH_COND[mn]}); break;  /* {mn} rel */")
        return

    # --- implied (accumulator forms of the shifts are handled below)
    if mode == "imp":
        body = IMPLIED_BODY[mn]
        if body:
            w(f"        case 0x{op:02X}: {body}; CYCLES({cyc}); break;  /* {mn} */")
        else:
            w(f"        case 0x{op:02X}: CYCLES({cyc}); break;  /* {mn} */")
        return

    # --- JMP / JSR
    if mn == "JMP":
        if mode == "abs":
            w(f"        case 0x{op:02X}: pc = ABS_(); CYCLES(3); break;  /* JMP abs */")
        else:
            w(f"        case 0x{op:02X}: JMP_IND(); CYCLES(5); break;  /* JMP (ind) */")
        return
    if mn == "JSR":
        w(f"        case 0x{op:02X}: {{ uint16_t t = ABS_(); PUSH16((uint16_t)(pc - 1));"
          f" pc = t; CYCLES(6); }} break;  /* JSR abs */")
        return

    # --- moves with a memory operand
    if mn in READ_OPS:
        if mn == "LAX":                       # one read, into A and X
            w(f"        case 0x{op:02X}: {{ uint8_t v = RD({a}); a = v; x = v;"
              f" SETZN(v); }} CYCLES({c}); break;")
            return
        reg = LOAD_REG.get(mn)
        if reg:
            w(f"        case 0x{op:02X}: {reg} = RD({a}); SETZN({reg});"
              f" CYCLES({c}); break;  /* {mn} {mode} */")
        else:
            # the operand goes into a local first: SETZN() inside the ALU
            # macros uses its argument twice, so handing it a side-effecting
            # RD(...) expression would read memory twice. The old form,
            # A_ADC(rd(imm())), evaluated the function argument once; the
            # local reproduces exactly that.
            w(f"        case 0x{op:02X}: {{ uint8_t v = RD({a}); {ALU_OP[mn]}(v); }}"
              f" CYCLES({c}); break;  /* {mn} {mode} */")
        return

    # --- stores
    if mn in ("STA", "STX", "STY", "SAX"):
        # SAX stores the AND of A and X without touching any flag
        reg = ("(uint8_t)(a & x)" if mn == "SAX" else STORE_REG[mn])
        w(f"        case 0x{op:02X}: WR({a}, {reg}); CYCLES({cyc}); break;  /* {mn} {mode} */")
        return

    # --- read-modify-write (single op, memory)
    if mn in ("ASL", "LSR", "ROL", "ROR", "INC", "DEC"):
        fn = SHIFT_FN[mn]
        if mode == "acc":
            w(f"        case 0x{op:02X}: a = {fn}(a); CYCLES(2); break;  /* {mn} A */")
        else:
            w(f"        case 0x{op:02X}: {{ uint16_t e = {a}; WR(e, {fn}(RD(e))); }}"
              f" CYCLES({cyc}); break;  /* {mn} {mode} */")
        return

    # --- combined illegal RMW+ALU ops
    if mn in RMW_ALU:
        step, after = RMW_ALU[mn]
        w(f"        case 0x{op:02X}: {{ uint16_t e = {a}; uint8_t v = {step}(RD(e));"
          f" WR(e, v); {after}(v); }} CYCLES({cyc}); break;  /* {mn} {mode} */")
        return

    # --- BIT: one read, then N/V come from the operand and Z from A&operand
    if mn == "BIT":
        w(f"        case 0x{op:02X}: {{ uint8_t v = RD({a}); p = (uint8_t)((p & ~(F_N | F_V | F_Z))"
          f" | (v & (F_N | F_V)) | ((a & v) ? 0 : F_Z)); }} CYCLES({cyc}); break;  /* BIT {mode} */")
        return

    raise KeyError((mn, mode))


def build():
    table = [None] * 256
    for mn, modes in OPS.items():
        for mode, op in modes.items():
            assert table[op] is None, f"opcode {op:02X} defined twice"
            table[op] = (mn, mode)
    cases = []
    out = cases.append
    for op in range(256):
        if table[op] is None:
            # undocumented opcode: behave as a 2-cycle NOP
            out(f"        case 0x{op:02X}: CYCLES(2); break;  /* illegal -> NOP */")
        else:
            emit_case(table[op][0], table[op][1], op, out)

    lines = []
    out = lines.append
    out("/* GENERATED by tools/gen_6502.py — do not edit by hand. */")
    out("/*")
    out(" * The instruction loop of cpu_run(), split into one case per opcode.")
    out(" * Every case is written in the macros cpu6502.c defines over the")
    out(" * automatic variables of that function — FETCH8()/RD()/WR(),")
    out(" * IMM()/ZP()/ABX()/..., A_ORA()/A_ADC()/SETZN()/..., CYCLES(),")
    out(" * PUSH()/PULL()/BRANCH_TAKE()/JMP_IND() — so the registers stay")
    out(" * locals of the loop and the compiler can hold them in host")
    out(" * registers; that is why the generator emits the loop itself and")
    out(" * not only the case labels (a #include cannot sit in a macro).")
    out(" *")
    out(" * Operand macros appear exactly once per case, in the order the old")
    out(" * function-call form evaluated them, so nothing with a side effect")
    out(" * (INX(), RD(), PULL()) is evaluated twice. `page` is set by the")
    out(" * addressing macro and read by CYCLES(n + page) in the same case.")
    out(" */")
    out("")
    out("    while (done < budget) {")
    out("        cyc = 0;")
    out("        page = 0;")
    out("        switch (FETCH8()) {")
    lines.extend(cases)
    out("        }")
    out("        if (cyc == 0) cyc = 2;")
    out("        if (cpu_stall) {")
    out("            cyc += cpu_stall;         /* OAM DMA: real halted cycles */")
    out("            cpu_stall = 0;")
    out("        }")
    out("        cpu.cycles += (uint32_t)cyc;")
    out("        done += cyc;")
    out("        ins++;")
    out("    }")
    return "\n".join(lines) + "\n", table


def main():
    code, table = build()
    if "--check" in sys.argv:
        try:
            cur = open("src/cpu_ops.h").read()
        except OSError:
            print("cpu_ops.h missing")
            return 1
        if cur == code:
            print("cpu_ops.h is up to date")
            return 0
        print("cpu_ops.h DIFFERS from the generator")
        return 1
    sys.stdout.write(code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
