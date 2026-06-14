"""R3000A (MIPS I) instruction decoder for the static recompiler.

Decode is the highest-leverage component: a decoder bug silently corrupts every
generated function, so this is developed test-first (see test_decode.py) and tracks
explicit opcode coverage. It produces a typed `Instr`; the emitter (S1) renders ops
to C. No emission or CPU modeling lives here — decode only.

R3000A encodings:
  R-type:  op(6) rs(5) rt(5) rd(5) shamt(5) funct(6)
  I-type:  op(6) rs(5) rt(5) imm(16)
  J-type:  op(6) target(26)
COP2 (opcode 0x12) is the GTE: rs<0x10 selects mfc2/cfc2/mtc2/ctc2; bit25 set => a
GTE command (the operation is the low 25 bits) — kept whole for the GTE module.
"""
from __future__ import annotations
from dataclasses import dataclass, field

REG = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]

# Instruction "kind" categories, used by the emitter to pick a translation template.
ALU_RRR = "alu_rrr"      # rd = rs OP rt        (add, sub, and, or, slt, ...)
ALU_RRI = "alu_rri"      # rt = rs OP imm       (addi, andi, ori, slti, ...)
SHIFT_I = "shift_i"      # rd = rt SH shamt     (sll, srl, sra)
SHIFT_V = "shift_v"      # rd = rt SH rs        (sllv, srlv, srav)
LUI = "lui"             # rt = imm << 16
MULDIV = "muldiv"        # hi/lo = rs OP rt     (mult, multu, div, divu)
HILO = "hilo"           # mfhi/mflo/mthi/mtlo
LOAD = "load"           # rt = mem[rs+imm]
STORE = "store"         # mem[rs+imm] = rt
BRANCH = "branch"        # conditional, PC-relative (beq, bne, bltz, ...)
JUMP = "jump"           # j / jal (absolute, in-region)
JUMPR = "jumpr"          # jr / jalr (register target)
SYSCALL = "syscall"
BREAK = "break_"
COP0 = "cop0"           # mfc0/mtc0/rfe
GTE_MOVE = "gte_move"    # mfc2/cfc2/mtc2/ctc2
GTE_OP = "gte_op"        # GTE command (RTPS, NCLIP, ...)
GTE_LOAD = "gte_load"    # lwc2
GTE_STORE = "gte_store"  # swc2
NOP = "nop"
UNKNOWN = "unknown"


@dataclass
class Instr:
    addr: int
    raw: int
    kind: str
    op: str = ""             # mnemonic, e.g. "addiu", "sw", "beq"
    rs: int = 0
    rt: int = 0
    rd: int = 0
    shamt: int = 0
    imm: int = 0             # raw 16-bit (unsigned) for I-type
    target: int = 0          # absolute target for j/jal and branch (resolved)
    cop2: int = 0            # full 32-bit word for GTE_OP (command)
    fields: dict = field(default_factory=dict)

    @property
    def simm(self) -> int:
        return self.imm - 0x10000 if self.imm & 0x8000 else self.imm


# opcode 0x00 SPECIAL: funct -> (op, kind)
SPECIAL = {
    0x00: ("sll", SHIFT_I), 0x02: ("srl", SHIFT_I), 0x03: ("sra", SHIFT_I),
    0x04: ("sllv", SHIFT_V), 0x06: ("srlv", SHIFT_V), 0x07: ("srav", SHIFT_V),
    0x08: ("jr", JUMPR), 0x09: ("jalr", JUMPR),
    0x0C: ("syscall", SYSCALL), 0x0D: ("break", BREAK),
    0x10: ("mfhi", HILO), 0x11: ("mthi", HILO),
    0x12: ("mflo", HILO), 0x13: ("mtlo", HILO),
    0x18: ("mult", MULDIV), 0x19: ("multu", MULDIV),
    0x1A: ("div", MULDIV), 0x1B: ("divu", MULDIV),
    0x20: ("add", ALU_RRR), 0x21: ("addu", ALU_RRR),
    0x22: ("sub", ALU_RRR), 0x23: ("subu", ALU_RRR),
    0x24: ("and", ALU_RRR), 0x25: ("or", ALU_RRR),
    0x26: ("xor", ALU_RRR), 0x27: ("nor", ALU_RRR),
    0x2A: ("slt", ALU_RRR), 0x2B: ("sltu", ALU_RRR),
}

# opcode 0x01 REGIMM: rt -> op (all BRANCH)
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}

# primary opcode -> (op, kind) for the simple I/J types
PRIMARY = {
    0x02: ("j", JUMP), 0x03: ("jal", JUMP),
    0x04: ("beq", BRANCH), 0x05: ("bne", BRANCH),
    0x06: ("blez", BRANCH), 0x07: ("bgtz", BRANCH),
    0x08: ("addi", ALU_RRI), 0x09: ("addiu", ALU_RRI),
    0x0A: ("slti", ALU_RRI), 0x0B: ("sltiu", ALU_RRI),
    0x0C: ("andi", ALU_RRI), 0x0D: ("ori", ALU_RRI), 0x0E: ("xori", ALU_RRI),
    0x0F: ("lui", LUI),
    0x20: ("lb", LOAD), 0x21: ("lh", LOAD), 0x22: ("lwl", LOAD),
    0x23: ("lw", LOAD), 0x24: ("lbu", LOAD), 0x25: ("lhu", LOAD),
    0x26: ("lwr", LOAD),
    0x28: ("sb", STORE), 0x29: ("sh", STORE), 0x2A: ("swl", STORE),
    0x2B: ("sw", STORE), 0x2E: ("swr", STORE),
    0x32: ("lwc2", GTE_LOAD), 0x3A: ("swc2", GTE_STORE),
}

# COP2 (opcode 0x12) rs<0x10 sub-ops
COP2_MOVE = {0x00: "mfc2", 0x02: "cfc2", 0x04: "mtc2", 0x06: "ctc2"}
COP0_MOVE = {0x00: "mfc0", 0x04: "mtc0"}


def decode(addr: int, raw: int) -> Instr:
    if raw == 0:
        return Instr(addr, raw, NOP, "nop")
    op = (raw >> 26) & 0x3F
    rs = (raw >> 21) & 0x1F
    rt = (raw >> 16) & 0x1F
    rd = (raw >> 11) & 0x1F
    shamt = (raw >> 6) & 0x1F
    funct = raw & 0x3F
    imm = raw & 0xFFFF

    if op == 0x00:  # SPECIAL
        ent = SPECIAL.get(funct)
        if ent is None:
            return Instr(addr, raw, UNKNOWN, f"special:0x{funct:02X}",
                         rs=rs, rt=rt, rd=rd, shamt=shamt)
        name, kind = ent
        return Instr(addr, raw, kind, name, rs=rs, rt=rt, rd=rd, shamt=shamt)

    if op == 0x01:  # REGIMM (branches)
        name = REGIMM.get(rt)
        if name is None:
            return Instr(addr, raw, UNKNOWN, f"regimm:0x{rt:02X}", rs=rs, imm=imm)
        tgt = addr + 4 + (((imm - 0x10000) if imm & 0x8000 else imm) << 2)
        return Instr(addr, raw, BRANCH, name, rs=rs, imm=imm, target=tgt)

    if op == 0x10:  # COP0
        if rs == 0x10 and funct == 0x10:
            return Instr(addr, raw, COP0, "rfe")
        name = COP0_MOVE.get(rs)
        if name is None:
            return Instr(addr, raw, UNKNOWN, f"cop0:0x{rs:02X}", rt=rt, rd=rd)
        return Instr(addr, raw, COP0, name, rt=rt, rd=rd)

    if op == 0x12:  # COP2 (GTE)
        if raw & (1 << 25):  # GTE command
            return Instr(addr, raw, GTE_OP, "cop2", cop2=raw)
        name = COP2_MOVE.get(rs)
        if name is None:
            return Instr(addr, raw, UNKNOWN, f"cop2:0x{rs:02X}", rt=rt, rd=rd)
        return Instr(addr, raw, GTE_MOVE, name, rt=rt, rd=rd)

    ent = PRIMARY.get(op)
    if ent is None:
        return Instr(addr, raw, UNKNOWN, f"op:0x{op:02X}", rs=rs, rt=rt, imm=imm)
    name, kind = ent

    if kind == JUMP:  # j / jal — target = (PC+4)[31:28] | (target26 << 2)
        tgt = ((addr + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)
        return Instr(addr, raw, kind, name, target=tgt)
    if kind == BRANCH:  # beq/bne/blez/bgtz — PC-relative
        tgt = addr + 4 + (((imm - 0x10000) if imm & 0x8000 else imm) << 2)
        return Instr(addr, raw, kind, name, rs=rs, rt=rt, imm=imm, target=tgt)
    # ALU_RRI / LUI / LOAD / STORE / GTE_LOAD / GTE_STORE
    return Instr(addr, raw, kind, name, rs=rs, rt=rt, imm=imm)


def fmt(ins: Instr) -> str:
    """Human-readable disassembly (for tests / dumps), not emitted C."""
    R = lambda n: REG[n]
    k, o = ins.kind, ins.op
    if k == NOP:
        return "nop"
    if k == LUI:
        return f"lui {R(ins.rt)}, 0x{ins.imm:04X}"
    if k == ALU_RRR:
        return f"{o} {R(ins.rd)}, {R(ins.rs)}, {R(ins.rt)}"
    if k == ALU_RRI:
        return f"{o} {R(ins.rt)}, {R(ins.rs)}, {ins.simm}"
    if k == SHIFT_I:
        return f"{o} {R(ins.rd)}, {R(ins.rt)}, {ins.shamt}"
    if k == SHIFT_V:
        return f"{o} {R(ins.rd)}, {R(ins.rt)}, {R(ins.rs)}"
    if k == MULDIV:
        return f"{o} {R(ins.rs)}, {R(ins.rt)}"
    if k == HILO:
        return f"{o} {R(ins.rd or ins.rs)}"
    if k in (LOAD, STORE, GTE_LOAD, GTE_STORE):
        return f"{o} {R(ins.rt)}, {ins.simm}({R(ins.rs)})"
    if k == BRANCH:
        if o in ("beq", "bne"):
            return f"{o} {R(ins.rs)}, {R(ins.rt)}, 0x{ins.target:08X}"
        return f"{o} {R(ins.rs)}, 0x{ins.target:08X}"
    if k == JUMP:
        return f"{o} 0x{ins.target:08X}"
    if k == JUMPR:
        return f"jalr {R(ins.rd)}, {R(ins.rs)}" if o == "jalr" else f"jr {R(ins.rs)}"
    if k == GTE_MOVE:
        return f"{o} {R(ins.rt)}, ${ins.rd}"
    if k == GTE_OP:
        return f"cop2 0x{ins.cop2 & 0x01FFFFFF:07X}"
    if k == COP0:
        return o if o == "rfe" else f"{o} {R(ins.rt)}, ${ins.rd}"
    if k in (SYSCALL, BREAK):
        return o
    return f"<{o}>"
