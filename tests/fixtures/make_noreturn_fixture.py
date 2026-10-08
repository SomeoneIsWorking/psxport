#!/usr/bin/env python3
"""Build the no-return fixture, v2. Generated, never committed.

WHY v1 FAILED, and it is the useful part. v1's "genuinely non-returning" leaf was a tight infinite
loop `b .; nop`, and the whole disassembly sweep entered it and never came out: **2 of the fixture's
instructions were ever decoded**, 0 functions were found at the call targets, and the analyzer
therefore had nothing to mislabel. It reported "marked 0", which read as "the analyzer does not
mislabel" -- a clean-looking zero produced by an instrument that scanned nothing. The fifth dead tap
in this workspace's own family, and this time inside the instrument built to end that family.

So v2 changes the SHAPE, not the intent:

  * the non-returning leaf BRANCHES (to a word that is not a call, and back), so the sweep keeps
    producing instructions instead of parking. It still never reaches a `jr ra`, which is what
    "non-returning" means to the analyzer.
  * a boot stub jumps to the first function, because the Raw Binary loader defines no entry point and
    without one "Disassemble Entry Points" has nothing to start from -- MEASURED on this Ghidra, and
    the reason the real pipeline seeds the window itself.
  * every function is separated by zero words, so one function's `jr ra` delay slot cannot swallow
    the next function's entry. v1's `noreturn_spin` was 3 words and its caller started 12 bytes in,
    which is how the delay slot ate the neighbour.
  * the control pair is kept and is the point: `noreturn_spin` genuinely never returns and
    `plain_leaf` is an ordinary leaf. If a clear removes BOTH, the policy is destroying information
    and a test that only counted "0 remaining" would call that a pass.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import struct

BASE = 0x80010000
NOP = 0x00000000
JR_RA = 0x03E00008
B = 0x1000FFFF  # `b .` -- a branch to the word after the delay slot

# Every function is placed in its own SLOT so no delay slot can reach the next entry.
SLOT_WORDS = 12
NAMES = [
    # (name, words, note about what it is FOR)
    ("noreturn_spin", [B, NOP, B, NOP, B, NOP, B, NOP],
     "branches forever and never reaches jr ra: the TRUE positive"),
    ("plain_leaf", [0x00001080, 0x00854021, JR_RA, NOP],
     "a0*2+1 then return: the ordinary leaf the incident is about"),
    ("tailcall_caller", None, "ends in a call, so nothing after the call site is reachable"),
    ("spin_caller", None, "calls the spinner and then has its own return path"),
    ("normal_caller", None, "calls the ordinary leaf and then returns: the control"),
    ("second_spin_caller", None, "a second, independent call site of the spinner"),
]


def w(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def jal(target: int) -> int:
    return 0x0C000000 | (((target & 0x0FFFFFFF) >> 2) & 0x03FFFFFF)


def j(target: int) -> int:
    return 0x08000000 | ((target & 0x0FFFFFFF) >> 2)


def slot_at(index: int) -> int:
    """Guest address of slot ``index``. Slot 0 is the boot stub."""
    return BASE + index * SLOT_WORDS * 4


def build() -> tuple[bytes, dict[str, int], dict[str, list[int]]]:
    names = {name: slot_at(index) for index, (name, _w, _n) in enumerate(NAMES)}
    bodies: dict[str, list[int]] = {}

    # slot 0: the boot stub. `j` + a nop delay slot, like a real PS-X EXE's.
    stub = [j(names["noreturn_spin"]), NOP]

    # tailcall_caller: a call as its LAST executed word, so the call site has no continuation.
    bodies["tailcall_caller"] = [0x27BDFFE0, jal(names["plain_leaf"]), 0x27BD0008,
                                 JR_RA, NOP, NOP, jal(names["plain_leaf"])]
    # spin_caller: the spinner, then a real return path of its own.
    bodies["spin_caller"] = [0x27BDFFE0, jal(names["noreturn_spin"]), 0x27BD0008,
                             JR_RA, NOP]
    # normal_caller: the ordinary leaf, then a return.
    bodies["normal_caller"] = [0x27BDFFE0, jal(names["plain_leaf"]), 0x00844021,
                               0x27BD0008, JR_RA, NOP]
    # second_spin_caller: a second call site of the same spinner, so the marking is not a one-off.
    bodies["second_spin_caller"] = [0x27BDFFE0, jal(names["noreturn_spin"]), 0x27BD0008,
                                    JR_RA, NOP]
    bodies["noreturn_spin"] = NAMES[0][1]
    bodies["plain_leaf"] = NAMES[1][1]

    text = bytearray()
    all_slots = [("stub", stub)] + [(n, bodies[n]) for n in names if n in bodies]
    for _name, words in all_slots:
        assert len(words) <= SLOT_WORDS, f"{_name} does not fit its slot"
        for word in words:
            text.extend(w(word))
        while len(text) % (SLOT_WORDS * 4):
            text.append(0)
    return bytes(text), names, bodies


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent
    image, names, bodies = build()
    (here / "noreturn.bin").write_bytes(image)
    descriptor = {
        "base": "0x%08X" % BASE,
        "size": len(image),
        "sha1": hashlib.sha1(image).hexdigest(),
        "functions": {n: "0x%08X" % a for n, a in names.items()},
        "expectations": {n: note for n, _w, note in NAMES},
    }
    (here / "noreturn.json").write_text(json.dumps(descriptor, indent=2, sort_keys=True) + "\n")
    print("wrote %d bytes, %d functions (+ a boot stub)" % (len(image), len(names)))
    for name, address in sorted(names.items(), key=lambda kv: kv[1]):
        print("  %-20s 0x%08X  %s" % (name, address,
                                       " ".join("0x%08X" % x for x in bodies[name])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
