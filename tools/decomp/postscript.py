"""Ghidra headless POST-script: clear the no-return guess, inventory the functions, decompile targets.

This file is run BY GHIDRA, inside Ghidra's own interpreter, loaded by path with no package context.
Two consequences shape it:

  * It imports nothing at module level, so it can be imported by a plain CPython test that exercises
    the decision logic below without a Ghidra install. The Ghidra imports are inside :func:`main`.
  * It cannot import this repository's modules, so the one rule it owns (what counts as a body
    actually being present) lives HERE and nowhere else.

ARGUMENTS (as built by :meth:`tools.decomp.headless.GhidraInvocation.command`)::

    <inventory.json> <c-dir> <noreturn-policy> <targets-file>

``<noreturn-policy>`` is ``all`` or a comma/space separated list of hex entry addresses. The
pipeline always passes ``all``; this script still accepts a list because a caller that has measured
a genuine non-returning function may need one, and it is the pipeline that refuses a partial policy
by default rather than this script quietly honouring it.

THE REASON THE POLICY EXISTS, unchanged in substance from the script this replaces: Ghidra's
non-returning-function analyzer guesses from call-site shape and on a PSX RAM dump it mislabels
ordinary leaf helpers. Every caller then decompiles to ``/* WARNING: Subroutine does not return */``
with the whole body after the call discarded and a fabricated ``return 0`` -- output that reads like
a complete function and is not one. Clearing the flag restores the real body. On a game RAM dump
genuine non-return is essentially absent and misanalysis is not, so ``all`` is the right default.
"""

from __future__ import annotations

import json
import re
import sys

# The decompiler's own marker for the failure this whole tool is built around. Matched as a phrase,
# not a whole comment, because the surrounding wording has changed between Ghidra releases and a
# stricter match would silently stop detecting it.
NORETURN_WARNING = "Subroutine does not return"

# A decompiled body that contains no `return` at all cannot be a complete function: a real one
# either returns a value, returns nothing, or is infinite. Used as a second, independent signal so
# a Ghidra that rewords the warning above is still caught by something.
RETURN_PATTERN = re.compile(r"\breturn\b")


class PolicyRefusal(Exception):
    """The no-return policy could not be read. Refused, never defaulted."""


def parse_noreturn_policy(value: str) -> tuple[str, frozenset[int]]:
    """``all`` or a list of hex addresses. Refuses anything else.

    A refused policy must not fall back to a default: the default is a behavioural choice about
    whether decompiled output is trustworthy, and guessing it is how a run reports complete-looking
    functions that are truncated.
    """
    text = (value or "").strip()
    if not text:
        raise PolicyRefusal("the no-return policy is empty. 'all' clears the guess on every function; "
                            "a list names the entry addresses to clear. An empty value would be "
                            "read as 'clear nothing', which is the failure this policy prevents.")
    if text.lower() == "all":
        return "all", frozenset()
    addresses: set[int] = set()
    for token in text.replace(",", " ").split():
        try:
            addresses.add(int(token, 16))
        except ValueError as error:
            raise PolicyRefusal(
                f"{token!r} in the no-return policy is not a hex address. Use 'all' or a list like "
                "'80012340,80012380'."
            ) from error
    if not addresses:
        raise PolicyRefusal("the no-return policy named no addresses.")
    return "list", frozenset(addresses)


def read_targets(path: str) -> list[int]:
    """The named target set, from a file of hex addresses, one per line or whitespace separated.

    ``#`` starts a comment that runs to the end of ITS LINE, not to the end of the next token:
    stripping a comment per token lets the words after a ``#`` be read as addresses, which is how a
    commented-out fixture reports targets nobody asked for.

    An ABSENT or EMPTY target file is refused. An empty target set would decompile nothing and the
    run would report success, which is precisely the "0 of 0 is a bug, not a result" case.
    """
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    except OSError as error:
        raise PolicyRefusal(
            f"cannot read the target list at {path}: {error}. Without targets the run decompiles "
            "nothing and would report success having established nothing."
        ) from error
    body = "\n".join(line.split("#", 1)[0] for line in raw.splitlines())
    tokens = body.replace(",", " ").split()
    if not tokens:
        raise PolicyRefusal(
            f"the target list at {path} is empty. Refusing: '0 of 0 targets decompiled' is a bug, "
            "not a result."
        )
    addresses: list[int] = []
    for token in tokens:
        try:
            addresses.append(int(token, 16))
        except ValueError as error:
            raise PolicyRefusal(f"{token!r} in {path} is not a hex guest address.") from error
    return sorted(set(addresses))


def classify_body(function_found: bool, instruction_count: int, c_text: str | None) -> tuple[bool, str]:
    """Is a real body actually present? ``(yes/no, the reason in words)``.

    THE STRUCTURAL DEFENCE. A Ghidra function object can exist, decompile, and produce a C body that
    is missing everything after a call, so "Ghidra returned C" is not evidence that the C is the
    function. Three independent signals, any one of which is disqualifying:

      1. the function object exists at all;
      2. its body holds at least one instruction;
      3. the C carries no non-return warning AND contains a ``return``.

    Signal 3 is split into two because the warning's wording is Ghidra's and may change; the
    ``return`` test is ours and is what catches the reworded case.
    """
    if not function_found:
        return False, "no function object at this address"
    if instruction_count <= 0:
        return False, f"function body holds 0 of 0 instructions ({instruction_count})"
    if c_text is None:
        return False, "decompile produced no C at all"
    if NORETURN_WARNING in c_text:
        return False, ("C carries Ghidra's non-return warning, so the body after the call was "
                       "discarded and a return fabricated")
    if not RETURN_PATTERN.search(c_text):
        return False, "C has no return statement, so it is not a complete function body"
    return True, "function object, %d instruction(s), C with a return and no non-return warning" % (
        instruction_count)


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = getScriptArgs()  # noqa: F821 - provided by GhidraScript, absent under plain CPython
    if len(argv) < 4:
        print("REFUSED: this post-script needs <inventory.json> <c-dir> <policy> <targets-file>, "
              "got %r" % (argv,))
        return 2
    inventory_path, c_dir, policy_text, targets_path = argv[0], argv[1], argv[2], argv[3]
    mode, only_addresses = parse_noreturn_policy(policy_text)
    targets = read_targets(targets_path)

    from ghidra.app.decompiler import DecompInterface  # noqa: PLC0415 - Ghidra-only import
    from ghidra.util.task import ConsoleTaskMonitor  # noqa: PLC0415

    program = currentProgram  # noqa: F821 - provided by GhidraScript
    fm = program.getFunctionManager()
    listing = program.getListing()
    address_factory = program.getAddressFactory()
    monitor = ConsoleTaskMonitor()

    def address_of(value: int):
        # NOT toAddr(value): these are KSEG0 addresses (>= 0x80000000) and the int overload
        # overflows Java's signed int. The hex-string factory overload is width-agnostic.
        return address_factory.getAddress("%08x" % value)

    # -- the no-return guess, cleared ---------------------------------------------------------------
    functions = list(fm.getFunctions(True))
    still_marked: list[str] = []
    cleared = 0
    for function in functions:
        if not function.hasNoReturn():
            continue
        offset = function.getEntryPoint().getOffset()
        if mode == "all" or offset in only_addresses:
            function.setNoReturn(False)
            cleared += 1
    # READ BACK, do not assume. A setNoReturn that silently failed would leave every caller
    # truncated while the inventory claimed the policy was applied.
    for function in fm.getFunctions(True):
        if function.hasNoReturn():
            still_marked.append("%08X" % function.getEntryPoint().getOffset())

    decompiler = DecompInterface()
    decompiler.toggleCCode(True)
    decompiler.openProgram(program)

    def bounded_count(function) -> int:
        """Instructions in the function's body.

        Counted by asking the listing for the body's OWN address set, not by walking linearly from
        the body's first address and stopping at its last. Two reasons, and the second was MEASURED:
        a linear walk attributes the next function's instructions to this one when the body does not
        extend to the last address, and it also MISCOUNTS a body that is not contiguous -- on Spyro
        1's FUN_800258F0 it reported 4,994 where the 19,980-byte body holds 4,995, and the
        cross-check against the image's own bytes is what found it. `getInstructions(AddressSetView,
        forward)` already handles a disjoint body correctly, so there is no reason to walk at all.
        """
        body = function.getBody()
        total = 0
        iterator = listing.getInstructions(body, True)
        while iterator.hasNext():
            iterator.next()
            total += 1
        return total

    # -- the function inventory ---------------------------------------------------------------------
    inventory = []
    for function in functions:
        body = function.getBody()
        inventory.append({
            "entry": "0x%08X" % function.getEntryPoint().getOffset(),
            "name": function.getName(),
            "body_bytes": body.getNumAddresses(),
            "body_first": "0x%08X" % body.getMinAddress().getOffset(),
            "body_last": "0x%08X" % body.getMaxAddress().getOffset(),
            "instruction_count": bounded_count(function),
        })

    # -- the named target set ----------------------------------------------------------------------
    import os  # noqa: PLC0415

    os.makedirs(c_dir, exist_ok=True)
    results = []
    for offset in targets:
        address = address_of(offset)
        function = fm.getFunctionAt(address)
        created = False

        # A TARGET THAT IS NOT A FUNCTION ENTRY MUST BE REFUSED, NOT CARVED. Measured on Spyro 1:
        # 0x800259FC is a phase INSIDE FUN_800258F0's body (0x800258F0..0x8002A6FB), and creating a
        # function there produced a second 4927-instruction body overlapping the first by 19708 of
        # its 19976 bytes. Both then decompiled to ~129 KB of C, both reported "body present", and
        # neither was a function. That is this workspace's signature failure reproduced by the tool
        # meant to catch it, so the enclosing body is named and the target refused.
        owner = None
        if function is None:
            containing = fm.getFunctionContaining(address)
            if containing is not None:
                owner = containing
            else:
                # Auto-analysis only defines a function where it found a call or branch to it, so a
                # valid entry reached only through a jump TABLE has no Function. Create it on demand.
                function = createFunction(address, None)  # noqa: F821 - GhidraScript
                created = function is not None

        record = {
            "requested": "0x%08X" % offset,
            "function_found": function is not None,
            "created_on_demand": created,
            "c_path": None,
            "c_bytes": 0,
            "instruction_count": 0,
            "body_bytes": 0,
            "decompiled": False,
            "decompile_error": None,
        }
        # The owner case is checked FIRST, and the order is load-bearing: when a target sits inside
        # another function's body there is no function at the address, so a `function is None`
        # branch placed above this one swallows the case and reports the generic "not code" message.
        # That is how the first version answered for 0x800259FC, and it named the wrong thing.
        if owner is not None:
            body = owner.getBody()
            record.update({
                "function_found": False,
                "inside_function": "%08X" % owner.getEntryPoint().getOffset(),
                "inside_function_name": owner.getName(),
                "inside_body_range": "0x%08X..0x%08X" % (body.getMinAddress().getOffset(),
                                                          body.getMaxAddress().getOffset()),
                "offset_into_body": offset - body.getMinAddress().getOffset(),
                "body_present": False,
                "body_reason": (
                    "this address is +%d bytes INSIDE %s's body (0x%08X..0x%08X), so it is a label "
                    "reached by a branch, not a function entry. Creating a function here would split "
                    "the enclosing body and produce two overlapping decompilations that both read as "
                    "complete. Read the enclosing function instead."
                    % (offset - body.getMinAddress().getOffset(), owner.getName(),
                       body.getMinAddress().getOffset(), body.getMaxAddress().getOffset())),
            })
            results.append(record)
            continue
        if function is None:
            record["body_present"] = False
            record["body_reason"] = (
                "no function at this address, and it is not inside another function's body either, "
                "so auto-analysis had nothing here and one could not be created. This establishes "
                "only that there is no function; it does not say why.")
            results.append(record)
            continue
        record["name"] = function.getName()
        record["body_bytes"] = function.getBody().getNumAddresses()
        record["instruction_count"] = bounded_count(function)
        result = decompiler.decompileFunction(function, 90, monitor)
        c_text = None
        if result is not None and result.decompileCompleted():
            c_text = result.getDecompiledFunction().getC()
        else:
            record["decompile_error"] = (result.getErrorMessage() if result else "no result object")
        if c_text is not None:
            c_file = os.path.join(c_dir, "%08X.c" % offset)
            with open(c_file, "w", encoding="utf-8") as handle:
                handle.write(c_text)
            record["c_path"] = os.path.relpath(c_file, os.path.dirname(inventory_path) or ".")
            record["c_bytes"] = len(c_text)
            record["decompiled"] = True
        present, reason = classify_body(record["function_found"], record["instruction_count"], c_text)
        record["body_present"] = present
        record["body_reason"] = reason
        results.append(record)

    # What the PRE-script did, if it ran. Absent means the pre-script raised or never ran, and
    # Ghidra carried the analysis on regardless -- so the audit refuses rather than the run
    # reporting "Post-analysis succeeded" over a seed that never happened.
    preseed = None
    preseed_path = os.environ.get("PSXPORT_DECOMP_PRESEED", "")
    if preseed_path and os.path.isfile(preseed_path):
        with open(preseed_path, "r", encoding="utf-8") as handle:
            preseed = json.load(handle)

    document = {
        "program": program.getName(),
        "language": str(program.getLanguageID()),
        "noreturn_policy": mode,
        "noreturn_cleared": cleared,
        "functions_scanned": len(functions),
        "noreturn_still_marked": still_marked,
        "preseed": preseed,
        "inventory": inventory,
        "targets_requested": len(targets),
        "targets": results,
    }
    with open(inventory_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")
    decompiler.dispose()
    print("POSTSCRIPT-OK functions=%d targets=%d cleared=%d still_marked=%d" % (
        len(functions), len(results), cleared, len(still_marked)))
    return 0


# THE ENTRY POINT, and getting it wrong is silent. Measured: with a `main()` and no call to it,
# Ghidra ran this file, found nothing to complain about, and reported
# "Post-analysis succeeded" while writing no inventory at all.
#
# PyGhidra loads a script as the module named '__main__' (pyghidra/script.py:251), so a top-level
# `if __name__ == "__main__"` DOES fire. What does NOT work is testing for the GhidraScript names to
# decide whether we are "inside Ghidra": `getScriptArgs` and `currentProgram` are provided by
# PyGhidraScript, a dict subclass whose `__missing__` falls back to the Java GhidraScript object, so
# a bare `getScriptArgs()` resolves while `"getScriptArgs" in globals()` is False. A guard written
# the second way silently never fires. `__name__` is the only reliable discriminator, and it is also
# what keeps this file importable by a plain CPython test.
if __name__ == "__main__":
    main()
