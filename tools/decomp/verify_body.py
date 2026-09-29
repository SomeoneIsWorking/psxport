#!/usr/bin/env python3
"""Check a decompiled body against the IMAGE'S OWN BYTES, from outside the tool that made it.

WHY THIS EXISTS. The body-presence column is the pipeline's claim about its own output, and a claim
is not evidence. This is the independent check: it decodes the exact instruction range the inventory
says the function holds, with the framework's neutral R3000A decoder (`tools/mips/decode.py`),
reading the PS-X EXE through the manifest's own file-offset formula, and then asserts the STRUCTURE
the decompiled C expresses against the structure those bytes contain.

WHAT IT CAN ESTABLISH, and what it cannot, because a diagnostic that cannot answer its question
must not pretend to:

  * CAN: the body's bounds describe a whole number of instructions; the body TERMINATES at a return;
    and every direct call site in the bytes has a corresponding call in the C. The last is the one
    that catches the failure this whole pipeline exists to prevent -- a caller decompiled with the
    body after a call discarded loses calls, and loses them silently.
  * CANNOT: that the instruction COUNT agrees. The count is Ghidra's, and a body may hold a word
    Ghidra does not hold as an instruction, so "words in the address range" and "instructions in the
    body" are different numbers that do not disagree about a defect. An earlier version of this file
    compared them anyway and reported a mismatch that was not real; the two are now printed, each
    named, and neither is a verdict.
  * CANNOT: that the decompiled C is the semantically correct program. Only a reading of both, by a
    person, establishes that, and the caller is expected to do it. So this prints the decoded listing
    beside the verdict, to make that reading possible.

IT NEEDS NO DECOMPILER. It reads the image, the inventory and the emitted C, so it can be run
against output from any source -- including output this pipeline produced a week ago.

    uv run --frozen python tools/decomp/verify_body.py \
        --inventory scratch/decomp/spyro1/inventory.json \
        --c scratch/decomp/spyro1/c/800122A8.c \
        --address 0x800122A8 \
        --image scratch/assets/spyro1/SCUS_942.28
"""

from __future__ import annotations

import argparse
import importlib
import json
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from decomp import images  # noqa: E402

# The MODULE, by explicit import: `import mips.decode as m` and `from mips import decode` BOTH
# resolve to the FUNCTION, because `mips/__init__.py` does `from .decode import decode`, which
# rebinds the package attribute `mips.decode` from the submodule to the function. importlib is the
# only one of the three forms that gets the module.
r3000a = importlib.import_module("mips.decode")

FUN_PATTERN = re.compile(r"\bFUN_([0-9a-fA-F]{8})\b")


class BodyRefusal(Exception):
    """The check could not run. Raised, never reported as agreement."""


def decode_body(window: images.ImageWindow, first: int, last: int) -> list:
    """Every 4-byte word in the body, decoded. Refuses a body that runs past the image rather than
    counting the part that is there and calling it the whole."""
    if (last - first + 1) % 4:
        raise BodyRefusal(f"body 0x{first:08X}..0x{last:08X} is not a whole number of instructions")
    words = []
    for cursor in range(first, last + 1, 4):
        if not window.covers(cursor, 4):
            raise BodyRefusal(
                f"body 0x{first:08X}..0x{last:08X} runs past the text window "
                f"0x{window.header.load:08X}..0x{window.text_end:08X}; the inventory's body is "
                "larger than the image")
        words.append((cursor, window.word(cursor), r3000a.decode(cursor, window.word(cursor))))
    return words


def direct_call_targets(words: list) -> list[int]:
    """Targets of the DIRECT call sites (`jal`).

    Filtered on the OPCODE, and case-insensitively: `jal` is a J-type, so `mips.decode` gives it a
    J-type target and a kind that is not `branch`, and filtering on `BRANCH` finds 0 call sites in a
    body that visibly contains ten. `mips.decode` spells opcodes in lower case, so an
    exact-case comparison against "JAL" finds 0 as well. Both were measured here, and both are the
    same class of bug as a control instrument that matches nothing and reads as a clean measurement.

    A `jalr` is a call through a register and has no name to check, so it is EXCLUDED rather than
    counted as a missing callee -- and the denominator printed says so, so "10 sites, 0 distinct"
    cannot be read as "the C called nothing".
    """
    return [ins.target for _c, _w, ins in words if ins.op.lower() == "jal"]


def terminates_with_return(words: list) -> tuple[bool, str]:
    """Does the body END by returning? A function that does not return is a real possibility on a
    PSX image, so this is reported as a finding with the words, not as a failure to hide."""
    if not words:
        return False, "the body is empty"
    _c, _w, last = words[-1]
    if len(words) >= 2:
        _c2, _w2, penultimate = words[-2]
        if penultimate.kind == r3000a.JUMPR and penultimate.target == 0 and last.kind == r3000a.NOP:
            return True, "ends `jr ra` + delay slot"
    kind, text = last.kind, r3000a.fmt(last)
    if last.kind in (r3000a.JUMP, r3000a.BRANCH):
        return False, f"ends by BRANCHING ({text}), not returning -- a tail-jump or a loop"
    return False, f"last instruction is {text}, which is not a return"


def check(inventory_path: Path, c_path: Path, address: int, image_path: Path,
          listing: bool = True) -> list[str]:
    """The problems found. EMPTY is the pass condition, and every entry names what was compared.

    ``listing`` prints the decoded listing, which is the point of the tool for a human reader and
    noise for anything driving it.
    """
    if not inventory_path.is_file():
        raise BodyRefusal(f"no inventory at {inventory_path}")
    if not c_path.is_file():
        raise BodyRefusal(f"no decompiled C at {c_path}")
    inventory = json.loads(inventory_path.read_text())
    c_text = c_path.read_text()

    row = next((f for f in inventory["inventory"] if int(f["entry"], 16) == address), None)
    if row is None:
        raise BodyRefusal(
            f"the inventory has no function at 0x{address:08X}. Nothing to check, which is not the "
            "same as passing.")
    target = next((t for t in inventory["targets"] if int(t["requested"], 16) == address), None)
    if target is not None and not target.get("body_present", False):
        raise BodyRefusal(
            f"the inventory itself says 0x{address:08X} has no body present "
            f"({target.get('body_reason', 'no reason recorded')}). Refusing to check a body the run "
            "already reported as absent.")

    specs = images.load_manifest()
    spec = next((s for s in specs.values() if s.serial == inventory["program"]), None)
    if spec is None:
        raise BodyRefusal(
            f"no manifest entry for serial {inventory['program']!r}, so there is no verified load "
            "geometry to read the bytes with.")
    window = images.ImageWindow(spec, image_path)

    body_first, body_last = int(row["body_first"], 16), int(row["body_last"], 16)
    declared = row["instruction_count"]
    words = decode_body(window, body_first, body_last)
    problems: list[str] = []

    # THE INSTRUCTION COUNT IS NOT CROSS-CHECKABLE HERE, and an earlier version of this file
    # claimed it was. It compared `len(decode_body(...))` -- every 4-byte WORD in the body's ADDRESS
    # RANGE -- against the inventory's instruction count, and reported Spyro 1's FUN_800258F0 as a
    # mismatch: 4,995 words against a claimed 4,994. That was NOT a defect in the inventory. A
    # function body may hold a word that Ghidra does not hold as an instruction (a word of data, or
    # one it never disassembled), so the two numbers measure different things and cannot disagree
    # about a defect. `mips.decode` decodes nearly every bit pattern, so it "decodes" that word too
    # and hides the difference. Telling which words Ghidra calls instructions needs Ghidra, which is
    # the thing this instrument exists to avoid.
    #
    # So the two numbers are printed, each NAMED, and neither is a pass/fail. The checks that CAN be
    # made are below: the body terminates, and every direct call site in the bytes has a call in the
    # C. An attractive wrong lead that survives into a report is worse than no lead, because the next
    # reader inherits it.
    range_words = (body_last - body_first + 1) // 4
    if (body_last - body_first + 1) % 4:
        problems.append(
            "the body's address range 0x%08X..0x%08X is %d bytes, which is not a whole number of "
            "instructions, so its bounds do not describe an instruction sequence."
            % (body_first, body_last, body_last - body_first + 1))

    unknown = [c for c, _w, ins in words if ins.kind == r3000a.UNKNOWN]
    if unknown:
        problems.append(
            "%d word(s) inside the body do not decode as MIPS (%s), so the body is not cleanly "
            "code as the inventory describes it."
            % (len(unknown), " ".join("0x%08X" % c for c in unknown[:8])))

    returned, how = terminates_with_return(words)
    if not returned:
        problems.append("the body does not end by returning: %s" % how)

    call_targets = direct_call_targets(words)
    c_fun = {int(m, 16) for m in FUN_PATTERN.findall(c_text)}
    missing = sorted(t for t in set(call_targets) if t not in c_fun)
    if missing:
        problems.append(
            "%d direct call site(s) in the bytes have no matching call in the decompiled C (%s). A "
            "caller decompiled with the body after a call discarded loses calls silently, and this "
            "is what that looks like."
            % (len(missing), " ".join("0x%08X" % t for t in missing[:8])))

    print("== %s ==" % c_path.name)
    print("body address range 0x%08X..0x%08X (%d bytes, %d words)"
          % (body_first, body_last, body_last - body_first + 1, range_words))
    print("inventory reports   %d instruction(s) -- NOT cross-checkable here; a body may hold a"
          % declared)
    print("                     word Ghidra does not hold as an instruction, and this tool cannot")
    print("                     see which those are without Ghidra.")
    print("words decoded       %d, %d not a recognised MIPS encoding"
          % (len(words), len(unknown)))
    print("direct call sites   %d (%d distinct targets; jalr through a register is excluded)"
          % (len(call_targets), len(set(call_targets))))
    print("C names FUN_        %d distinct" % len(c_fun))
    print("body terminates     %s" % how)
    if missing:
        print("MISSING FROM C    %s" % " ".join("0x%08X" % t for t in missing[:8]))
    print("")
    if not listing:
        return problems
    print("== decoded listing, for the reading this cannot do ==")
    for cursor, word, ins in words:
        print("  0x%08X  %08X  %s" % (cursor, word, r3000a.fmt(ins)))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--c", type=Path, required=True)
    parser.add_argument("--address", type=lambda t: int(t, 16), required=True)
    parser.add_argument("--image", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        problems = check(args.inventory, args.c, args.address, args.image)
    except (BodyRefusal, images.ImageRefusal) as error:
        print("REFUSED: %s" % error)
        return 1
    if problems:
        print("BODY CHECK FAILED -- %d problem(s):" % len(problems))
        for problem in problems:
            print("  - %s" % problem)
        return 2
    print("BODY CHECK OK: the decoded bytes and the decompiled C agree on the structure.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
