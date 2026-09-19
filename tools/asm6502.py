#!/usr/bin/env python3
"""
asm6502.py — a tiny two-pass 6502 assembler.

Written for this project so that every test program and test ROM in the
repository is generated from source we own (no downloaded binaries).

Supported syntax (one instruction per line):

    label:                 define a label
    LDA #$10               immediate            (also  LDA #label)
    LDA $10                zero page or absolute (auto-sized)
    LDA $0200   LDX $10,Y  LDA $0200,X
    LDA ($10),Y   LDA ($10,X)
    JMP ($FFFC)            indirect jump
    BNE label              relative branches
    JSR label / JMP label
    .org $C000             set the origin
    .byte $01,$02,"AB"     raw data
    .word $1234,label
    .res 16                reserve zero bytes
    ; comment   // comment

Usage:
    from asm6502 import Assembler
    a = Assembler(org=0x8000)
    a.line("start:", "LDA #$00", "TAX", "loop: JMP loop")
    image = a.assemble()
"""
from gen_6502 import OPS

BRANCHES = set("BPL BMI BVC BVS BCC BCS BNE BEQ".split())
IMPLIED = set("""BRK CLC SEC CLI SEI CLV CLD SED DEX DEY INX INY TAX TAY
                 TSX TXA TXS TYA NOP PHA PHP PLA PLP RTI RTS""".split())
ACC = set("ASL LSR ROL ROR".split())

MODE_SIZE = {"imp": 1, "acc": 1, "imm": 2, "zp": 2, "zpx": 2, "zpy": 2,
             "abs": 3, "abx": 3, "aby": 3, "inx": 2, "iny": 2,
             "ind": 3, "rel": 2}


class Assembler:
    def __init__(self, org=0x8000):
        self.org = org
        self.items = []       # ("op", mn, mode, token) | ("org", v) | ("byte", [tokens]) ...
        self.labels = {}
        self.pc = org

    # ------------------------------------------------------------- adding
    def line(self, *lines):
        for ln in lines:
            self.add(ln)
        return self

    def add(self, text):
        text = self._strip_comment(text).strip()
        if not text:
            return
        while ":" in text:
            name, rest = text.split(":", 1)
            name = name.strip()
            if name:
                if name in self.labels and self.labels[name] != self.pc:
                    raise SyntaxError(f"duplicate label {name}")
                self.labels[name] = self.pc
            text = rest.strip()
            if not text:
                return
        if text.startswith("."):
            self._directive(text)
            return
        parts = text.split(None, 1)
        mn = parts[0].upper()
        operand = parts[1].strip() if len(parts) > 1 else ""
        if not operand:
            if mn in IMPLIED:
                self._append(("op", mn, "imp", None), 1)
                return
            if mn in ACC:
                self._append(("op", mn, "acc", None), 1)
                return
            raise SyntaxError(f"{mn} needs an operand")
        if mn in ACC and operand.upper() == "A":        # ASL A / ROR A ...
            self._append(("op", mn, "acc", None), 1)
            return
        mode, token = self._parse_operand(mn, operand)
        size = MODE_SIZE["rel" if mn in BRANCHES else mode]
        self._append(("op", mn, mode, token), size)

    def _append(self, item, size):
        """Record an item and advance the program counter immediately, so
        that labels defined later in the source get the right address."""
        self.items.append(item)
        self.pc += size

    @staticmethod
    def _strip_comment(text):
        for marker in (";", "//"):
            i = text.find(marker)
            if i >= 0:
                text = text[:i]
        return text

    def _directive(self, text):
        parts = text.split(None, 1)
        d = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""
        if d == ".org":
            v = self._try_value(arg)
            if v is None:
                raise SyntaxError(".org needs a constant")
            self.items.append(("org", arg))
            self.pc = v
        elif d == ".byte":
            toks = self._split_args(arg)
            self._append(("byte", toks), sum(
                len(t.strip('"')) if t.startswith('"') else 1 for t in toks))
        elif d == ".word":
            toks = self._split_args(arg)
            self._append(("word", toks), 2 * len(toks))
        elif d == ".res":
            v = self._try_value(arg)
            if v is None:
                raise SyntaxError(".res needs a constant")
            self._append(("res", arg), v)
        else:
            raise SyntaxError(f"unknown directive {d}")

    @staticmethod
    def _split_args(arg):
        out, cur, instr = [], "", False
        for ch in arg:
            if ch == '"':
                instr = not instr
                cur += ch
            elif ch == "," and not instr:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    # ------------------------------------------------------------ parsing
    def _parse_operand(self, mn, op):
        op = op.strip()
        up = op.upper()
        if op.startswith("#"):
            return "imm", op[1:].strip()
        if op.startswith("(") and up.endswith(",X)"):
            return "inx", op[1:-3].strip()
        if op.startswith("(") and up.endswith("),Y"):
            return "iny", op[1:-3].strip()
        if op.startswith("(") and op.endswith(")"):
            return "ind", op[1:-1].strip()
        idx = None
        for suffix, tag in ((",X", "x"), (",Y", "y")):
            if up.endswith(suffix):
                idx = tag
                op = op[: -len(suffix)].strip()
        # decide zero page vs absolute
        v = self._try_value(op)
        zp_ok = v is not None and v < 0x100
        if idx is None:
            if zp_ok and "zp" in OPS.get(mn, {}):
                return "zp", op
            return "abs", op
        if idx == "x":
            if zp_ok and "zpx" in OPS.get(mn, {}):
                return "zpx", op
            return "abx", op
        if zp_ok and "zpy" in OPS.get(mn, {}):
            return "zpy", op
        return "aby", op

    def _try_value(self, tok):
        """Value if known now (numeric literal or already-defined label)."""
        tok = tok.strip()
        try:
            if tok.startswith("$"):
                return int(tok[1:], 16)
            if tok.startswith("%"):
                return int(tok[1:], 2)
            if tok[:1].isdigit():
                return int(tok, 0)
        except ValueError:
            return None
        if tok.startswith("<") or tok.startswith(">"):
            return None
        return self.labels.get(tok)

    def _value(self, tok):
        """Resolve during the emit pass (all labels known)."""
        tok = tok.strip()
        if tok.startswith("$"):
            return int(tok[1:], 16)
        if tok.startswith("%"):
            return int(tok[1:], 2)
        if tok.startswith("<"):
            return self._value(tok[1:]) & 0xFF
        if tok.startswith(">"):
            return (self._value(tok[1:]) >> 8) & 0xFF
        if tok[:1].isdigit():
            return int(tok, 0)
        if tok in self.labels:
            return self.labels[tok]
        raise SyntaxError(f"undefined label {tok}")

    # ------------------------------------------------------------ assembly
    def assemble(self):
        # single emit pass: sizes were fixed while parsing, all labels
        # are known by now, so tokens can be resolved for real
        self.pc = self.org
        out = bytearray()
        for item in self.items:
            kind = item[0]
            if kind == "org":
                v = self._value(item[1])
                if v > self.pc:
                    out += b"\x00" * (v - self.pc)
                self.pc = v
            elif kind == "byte":
                for t in item[1]:
                    if t.startswith('"'):
                        data = t.strip('"').encode("ascii")
                        out += data
                        self.pc += len(data)
                    else:
                        out.append(self._value(t) & 0xFF)
                        self.pc += 1
            elif kind == "word":
                for t in item[1]:
                    v = self._value(t)
                    out += bytes((v & 0xFF, (v >> 8) & 0xFF))
                    self.pc += 2
            elif kind == "res":
                n = self._value(item[1])
                out += b"\x00" * n
                self.pc += n
            else:
                _, mn, mode, token = item
                if mn in BRANCHES:
                    target = self._value(token)
                    off = target - (self.pc + 2)
                    if not -128 <= off <= 127:
                        raise SyntaxError(f"branch out of range at {self.pc:04X}")
                    out += bytes((OPS[mn]["rel"], off & 0xFF))
                    self.pc += 2
                    continue
                op = OPS[mn][mode]
                if mode in ("imp", "acc"):
                    out.append(op)
                    self.pc += 1
                elif mode in ("imm", "zp", "zpx", "zpy", "inx", "iny"):
                    out += bytes((op, self._value(token) & 0xFF))
                    self.pc += 2
                else:  # abs / abx / aby / ind
                    v = self._value(token)
                    out += bytes((op, v & 0xFF, (v >> 8) & 0xFF))
                    self.pc += 3
        return bytes(out)


def assemble_lines(lines, org=0x8000):
    a = Assembler(org)
    for ln in lines:
        a.add(ln)
    return a.assemble()
