"""The STRUCTURAL shape of a run of guest words: is this one function, or several merged?

ONE CONCEPT, and ONE implementation. Three call sites needed the same questions asked of the same
MIPS words — the post-script reporting a body's shape, the cross-check reading call sites, and the
overlay control asking whether an address holds an entry — and each of them was first written with
its own bit arithmetic. That is three copies of one rule, which drift exactly where a re-port drifts,
so the rule lives here and the framework's own decoder (`tools/mips/decode.py`) owns the ENCODING.
This module owns only the QUESTIONS.

WHAT A SHAPE CAN AND CANNOT ESTABLISH, because a diagnostic that cannot answer its question must not
pretend to:

  * CAN: how many entry prologues, jump-register forms, direct calls and indirect calls a run of
    words holds. Those are counts over real bytes.
  * CANNOT: whether a Ghidra function is one function or several the analyzer merged. NOTHING in
    Ghidra's API says so, and a threshold would be a guess about an image. So no verdict is issued
    here or anywhere downstream: `prologues > 1` inside one body is the SHAPE of a merge, and the
    judgement belongs to whoever reads the number.

THE ENTRY PROLOGUE, stated once. `addiu $sp, $sp, -N` is a non-leaf function's first instruction on
MIPS-I, it is the shape the vagrant repo's own `re_overlay.py` uses to derive module load bases from a
module's bytes, and it is what a code extent begins with. Matching it here rather than in three call
sites is the point of this file.
"""
from __future__ import annotations

import importlib

# The framework's neutral R3000A decoder. Imported by EXPLICIT module name: `mips/__init__.py` does
# `from .decode import decode`, which rebinds the package attribute `mips.decode` from the submodule
# to the function, so `import mips.decode as x` and `from mips import decode` both yield the FUNCTION.
r3000a = importlib.import_module("mips.decode")

SP = r3000a.REG.index("sp")
RA = r3000a.REG.index("ra")
ZERO = r3000a.REG.index("zero")


def is_entry_prologue(word: int) -> bool:
    """Is this word `addiu $sp, $sp, -N` — a non-leaf function's entry?

    The immediate must be NEGATIVE. `addiu $sp, $sp, +N` on a real entry is not a frame setup, and
    matching either sign would count every stack adjustment in a body as an entry point, which is the
    "matches when nothing should match" defect.
    """
    instruction = r3000a.decode(0, word)
    return (instruction.kind == r3000a.ALU_RRI and instruction.op == "addiu"
            and instruction.rt == SP and instruction.rs == SP and instruction.simm < 0)


def is_jump_register(word: int) -> bool:
    """`jr` (funct 0x08) or `jalr` (funct 0x09) — a transfer through a register."""
    instruction = r3000a.decode(0, word)
    return instruction.kind == r3000a.JUMPR and instruction.op in ("jr", "jalr")


def is_direct_call(word: int) -> bool:
    """`jal` to an absolute target."""
    instruction = r3000a.decode(0, word)
    return instruction.kind == r3000a.JUMP and instruction.op in ("jal", "bal")


def is_indirect_call(word: int) -> bool:
    """`jalr` through a register that is neither `zero` nor `ra` — how a PSX image calls a pointer."""
    instruction = r3000a.decode(0, word)
    if instruction.kind != r3000a.JUMPR or instruction.op != "jalr":
        return False
    return instruction.rs not in (ZERO, RA)


def shape(words) -> dict:
    """Count the structural features of a run of raw LE instruction words.

    `words` is a sequence of 32-bit little-endian instruction words, NOT bytes and NOT a Java list;
    callers holding a Ghidra AddressSet pass the values they read out of memory.
    """
    words = list(words)
    return {
        "words": len(words),
        "prologues": sum(1 for w in words if is_entry_prologue(w)),
        "returns": sum(1 for w in words if is_jump_register(w)),
        "calls": sum(1 for w in words if is_direct_call(w)),
        "indirect_calls": sum(1 for w in words if is_indirect_call(w)),
    }
