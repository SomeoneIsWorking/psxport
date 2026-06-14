"""Test-first decoder suite. Run: python3 -m pytest tools/recomp/test_decode.py -q
(falls back to a plain `python3 tools/recomp/test_decode.py` runner with no pytest).

Golden cases are hand-verified by bit-field, including the 8 real instructions at the
Tomba! 2 entry point (0x80018B6C) so the suite is anchored to real ROM bytes.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import decode as D
from decode import decode, fmt


def chk(addr, raw, kind, text, **flds):
    ins = decode(addr, raw)
    assert ins.kind == kind, f"0x{raw:08X}: kind {ins.kind!r} != {kind!r}"
    assert fmt(ins) == text, f"0x{raw:08X}: fmt {fmt(ins)!r} != {text!r}"
    for k, v in flds.items():
        assert getattr(ins, k) == v, f"0x{raw:08X}: {k}={getattr(ins,k)!r} != {v!r}"


def test_entrypoint_words():
    # Real Tomba!2 entry @0x80018B6C (verified against the EXE bytes).
    base = 0x80018B6C
    chk(base + 0x00, 0x3C028004, D.LUI, "lui v0, 0x8004", rt=2, imm=0x8004)
    chk(base + 0x04, 0x24428458, D.ALU_RRI, "addiu v0, v0, -31656", rs=2, rt=2)
    chk(base + 0x08, 0x3C038004, D.LUI, "lui v1, 0x8004", rt=3)
    chk(base + 0x0C, 0x2463C358, D.ALU_RRI, "addiu v1, v1, -15528", rs=3, rt=3)
    chk(base + 0x10, 0xAC400000, D.STORE, "sw zero, 0(v0)", rs=2, rt=0, imm=0)
    chk(base + 0x14, 0x24420004, D.ALU_RRI, "addiu v0, v0, 4", rs=2, rt=2)
    chk(base + 0x18, 0x0043082B, D.ALU_RRR, "sltu at, v0, v1", rs=2, rt=3, rd=1)
    # bne at, zero, -4 words -> target = (addr+4) + (-4<<2) = base+0x10 (loop top)
    chk(base + 0x1C, 0x1420FFFC, D.BRANCH, f"bne at, zero, 0x{base+0x10:08X}",
        rs=1, rt=0, target=base + 0x10)


def test_alu():
    chk(0, 0x00851020, D.ALU_RRR, "add v0, a0, a1", rs=4, rt=5, rd=2)
    chk(0, 0x00851021, D.ALU_RRR, "addu v0, a0, a1")
    chk(0, 0x00851023, D.ALU_RRR, "subu v0, a0, a1")
    chk(0, 0x00851024, D.ALU_RRR, "and v0, a0, a1")
    chk(0, 0x00851025, D.ALU_RRR, "or v0, a0, a1")
    chk(0, 0x0085102A, D.ALU_RRR, "slt v0, a0, a1")
    chk(0, 0x308400FF, D.ALU_RRI, "andi a0, a0, 255", imm=0xFF)
    chk(0, 0x3484FFFF, D.ALU_RRI, "ori a0, a0, -1")  # ori imm sign-shown; raw kept
    chk(0, 0x28820010, D.ALU_RRI, "slti v0, a0, 16")


def test_shift():
    chk(0, 0x00042080, D.SHIFT_I, "sll a0, a0, 2", rt=4, rd=4, shamt=2)
    chk(0, 0x00042082, D.SHIFT_I, "srl a0, a0, 2")
    chk(0, 0x00042083, D.SHIFT_I, "sra a0, a0, 2")
    chk(0, 0x00a42004, D.SHIFT_V, "sllv a0, a0, a1", rs=5, rt=4, rd=4)


def test_muldiv_hilo():
    chk(0, 0x00850018, D.MULDIV, "mult a0, a1", rs=4, rt=5)
    chk(0, 0x0085001B, D.MULDIV, "divu a0, a1")
    chk(0, 0x00001012, D.HILO, "mflo v0", rd=2)
    chk(0, 0x00001010, D.HILO, "mfhi v0", rd=2)


def test_mem():
    chk(0, 0x8C820010, D.LOAD, "lw v0, 16(a0)", rs=4, rt=2, imm=0x10)
    chk(0, 0x90820000, D.LOAD, "lbu v0, 0(a0)")
    chk(0, 0xA0820004, D.STORE, "sb v0, 4(a0)")
    chk(0, 0xA482FFFC, D.STORE, "sh v0, -4(a0)", imm=0xFFFC)


def test_jumps_branches():
    # jal 0x80018000: target26 = 0x80018000>>2 & 0x03FFFFFF
    raw = 0x0C000000 | ((0x80018000 >> 2) & 0x03FFFFFF)
    chk(0x80010000, raw, D.JUMP, "jal 0x80018000", target=0x80018000)
    chk(0, 0x03E00008, D.JUMPR, "jr ra", rs=31)
    chk(0, 0x0080F809, D.JUMPR, "jalr ra, a0", rs=4, rd=31)
    # beq a0,a1,+8 : target = 4+8 = 0xC at addr 0
    chk(0, 0x10850002, D.BRANCH, "beq a0, a1, 0x0000000C", target=0xC)
    chk(0, 0x04810002, D.BRANCH, "bgez a0, 0x0000000C", rs=4)  # regimm rt=1


def test_cop():
    chk(0, 0x40046000, D.COP0, "mfc0 a0, $12", rt=4, rd=12)  # mfc0 a0, SR
    chk(0, 0x42000010, D.COP0, "rfe")
    chk(0, 0x4884F800, D.GTE_MOVE, "mtc2 a0, $31", rt=4, rd=31)  # rs=4 mtc2
    chk(0, 0x48000000, D.GTE_MOVE, "mfc2 zero, $0")  # rs=0 mfc2
    # GTE command (bit25 set): RTPS = 0x4A180001
    ins = decode(0, 0x4A180001)
    assert ins.kind == D.GTE_OP and ins.cop2 == 0x4A180001
    chk(0, 0xC8820000, D.GTE_LOAD, "lwc2 v0, 0(a0)")
    chk(0, 0xE8820000, D.GTE_STORE, "swc2 v0, 0(a0)")


def test_nop_and_unknown():
    chk(0, 0x00000000, D.NOP, "nop")
    assert decode(0, 0xFC000000).kind == D.UNKNOWN  # opcode 0x3F unused


def _main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    fails = 0
    for f in fns:
        try:
            f()
            print(f"ok   {f.__name__}")
        except AssertionError as e:
            fails += 1
            print(f"FAIL {f.__name__}: {e}")
    print(f"\n{len(fns)-fails}/{len(fns)} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    _main()
