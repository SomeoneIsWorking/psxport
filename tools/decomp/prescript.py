"""Ghidra headless PRE-script: seed disassembly from the image's declared entry point.

WHY THIS EXISTS. Two measured facts, and the second is the reason the shape is what it is.

  1. The Raw Binary loader defines no entry point, so "Disassemble Entry Points" has nothing to
     start from. Measured on a synthetic LE MIPS image at 0x80010000: imported flat, auto-analysis
     produced **0 instructions and 0 functions** and logged "Analysis succeeded" and exited 0.
  2. A PS-X EXE *does* carry an entry point, in its header, and Ghidra's own analyzer uses it. So
     for a real image the correct seed is the header's entry -- NOT the start of the text window.

The first version of this file seeded from the window start and RAISED on Spyro 1, because the first
byte of a PS-X EXE's text window is DATA: `disassemble(0x80010000)` produces nothing, while the real
entry `0x8005B8E0` decompiles to 670 functions. The lesson is the general one, and it is why the
entry point is an explicit argument here rather than something this file guesses.

It records what it did in ``preseed.json``, which the post-script copies into the inventory and the
audit checks. That is the second lesson: a pre-script that raises leaves Ghidra running the analysis
anyway, logs "Post-analysis succeeded", and exits 0 -- so an unrecorded pre-script failure is
invisible in the result. Recording it is what makes it visible.

ARGUMENTS: ``<lo_hex> <hi_hex> <entry_hex>``, built by
:meth:`tools.decomp.headless.GhidraInvocation.command`.

Like the post-script, this imports nothing at module level so a plain CPython test can import it.
REFUSALS RAISE, because GhidraScript's return value is discarded and a script that "returns 3"
continues the run as if it had not checked anything.
"""

from __future__ import annotations

import json
import os


class PreScriptRefusal(Exception):
    """The declared entry point did not yield instructions. Raised so Ghidra reports it."""


def parse_window(argv: list[str]) -> tuple[int, int, int]:
    """``(lo, hi, entry)`` in hex, or refuse. A missing or malformed bound refuses rather than
    defaulting, because every downstream number is derived from these three."""
    if len(argv) < 3:
        raise PreScriptRefusal(
            "this pre-script needs <lo_hex> <hi_hex> <entry_hex>, got %r. All three come from the "
            "image's own PS-X EXE header, and the entry point is what disassembly is seeded from: "
            "seeding from the window start disassembles DATA on a real image." % (argv,))
    try:
        lo, hi, entry = (int(token, 16) for token in argv[0:3])
    except ValueError as error:
        raise PreScriptRefusal("window and entry must be hex, got %r" % (argv,)) from error
    if hi <= lo:
        raise PreScriptRefusal("text window 0x%08X..0x%08X is empty or reversed" % (lo, hi))
    if not lo <= entry <= hi:
        raise PreScriptRefusal(
            "the declared entry 0x%08X is outside the text window 0x%08X..0x%08X, so the image and "
            "the manifest describe different things." % (entry, lo, hi))
    return lo, hi, entry


# End of main RAM in KSEG0. A PS-X EXE's file image ends where its .bss begins; everything above is
# uninitialised guest RAM the program writes at runtime (heap, stack, module arenas).
KSEG0_LAST = 0x80200000


def map_bss_window(program, text_end: int) -> dict:
    """Map the uninitialised RAM past the file image, so references INTO it are analysable.

    Ghidra's Raw Binary loader maps exactly the file, so a resident image's mapped memory stops at
    its own length and every guest address above that is "outside the memory of the analyzed
    program" — which makes `refs` on a runtime word (a scene machine, a queue head, anything in
    .bss) answer "outside the program" instead of the references that name its writer. That is a
    statement about the LOADER, not about the binary, so it is fixed here for every resident image
    rather than per title: the guest's .bss genuinely is part of its address space.

    Disassembly is NOT seeded across this block: it is uninitialised, so anything decoded from it
    would be invented. It is mapped so references resolve, nothing more.
    """
    memory = program.getMemory()  # noqa: F821
    start = program.getAddressFactory().getAddress("%08x" % text_end)  # noqa: F821
    if text_end >= KSEG0_LAST:
        return {"mapped": False, "reason": "text already reaches the end of RAM"}
    # Ghidra refuses to create a block that overlaps an existing one, and it reports the overlap as an
    # exception. Rather than guess at a pre-check that can disagree with the real geometry, the create
    # IS the test: its own failure is the authoritative "already mapped" answer.
    try:
        block = memory.createUninitializedBlock(".bss", start, KSEG0_LAST - text_end, False)  # noqa: F821
    except Exception as error:  # noqa: BLE001 - Ghidra's own exception type is not on this classpath
        return {"mapped": False, "reason": "create refused: %s" % (error,)}
    block.setRead(True)
    block.setWrite(True)
    block.setExecute(False)
    block.setVolatile(False)
    block.setInitialized(False)
    return {"mapped": True, "start": "0x%08X" % text_end, "end": "0x%08X" % KSEG0_LAST}


def count_instructions(listing, first, last) -> int:
    """Instructions in the half-open ADDRESS window. Bounded, for the same reason the post-script
    bounds its per-function count: an unbounded walk attributes the next region's code to this one.

    ``first`` and ``last`` must be ``ghidra.program.model.address.Address`` values and NOT Python
    ints. Measured: ``listing.getInstructions(int, bool)`` raises "No matching overloads found ...
    options are: getInstructions(boolean), getInstructions(AddressSetView, boolean),
    getInstructions(Address, boolean)" -- the Python int is not narrowed to the Java Address
    overload. Passing the Address through is the fix, and the same trap as the sibling
    ``setAnalysisOption`` failure the spyro pre-script hit.
    """
    total = 0
    iterator = listing.getInstructions(first, True)
    while iterator.hasNext():
        if iterator.next().getAddress().compareTo(last) >= 0:
            break
        total += 1
    return total


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = getScriptArgs()  # noqa: F821 - provided by GhidraScript
    lo, hi, entry = parse_window(list(argv))

    bss = map_bss_window(currentProgram, hi + 1)  # noqa: F821  # `hi` is the last mapped byte

    factory = currentProgram.getAddressFactory()  # noqa: F821
    seeded_address = factory.getAddress("%08x" % entry)
    last_address = factory.getAddress("%08x" % hi)
    seeded = disassemble(seeded_address)  # noqa: F821 - GhidraScript; follows the flow
    listing = currentProgram.getListing()  # noqa: F821
    found = count_instructions(listing, seeded_address, last_address)

    marker = {
        "text_first": "0x%08X" % lo,
        "text_last": "0x%08X" % hi,
        "entry": "0x%08X" % entry,
        "disassemble_returned": bool(seeded),
        "instructions_from_entry": found,
        "bss_window": bss,
    }
    # Written beside the inventory so the post-script can report it and the audit can REFUSE an
    # absent or empty one. A pre-script that raises is otherwise invisible: Ghidra carries on and
    # the run reports "Post-analysis succeeded".
    out = os.environ.get("PSXPORT_DECOMP_PRESEED", "")
    if not out:
        raise PreScriptRefusal(
            "PSXPORT_DECOMP_PRESEED is not set, so what this pre-script did cannot be recorded and a "
            "failure here would be invisible in the result. The pipeline sets it.")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(marker, handle, indent=2, sort_keys=True)
        handle.write("\n")

    if found == 0:
        # The message states what was scanned and what was tried, and does NOT name a cause. An
        # earlier version of this refusal asserted "the load base is the first thing to check", and
        # the base was correct: the cause was seeding from data. An instrument that cannot answer
        # its question must not put a plausible one in the sentence instead.
        raise PreScriptRefusal(
            "scanned the text window 0x%08X..0x%08X and disassembled from the declared entry "
            "0x%08X, which produced 0 instructions. This establishes only that those bytes did not "
            "decode; it does NOT establish why. Check the entry point and the window against the "
            "image's own PS-X EXE header, and whether the bytes at the entry are code."
            % (lo, hi, entry))
    print("PRESCRIPT-OK entry=0x%08X window=0x%08X..0x%08X seeded=%s instructions=%d"
          % (entry, lo, hi, seeded, found))
    return 0


# See the sibling's entry-point note: PyGhidra loads this as '__main__', so this guard fires, and
# `__name__` is the only reliable discriminator (a `"getScriptArgs" in globals()` test is always
# False here, because PyGhidraScript is a dict subclass that resolves bare names through
# __missing__ rather than through real module contents).
if __name__ == "__main__":
    main()
