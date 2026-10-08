"""Ghidra headless QUERY script: answer "who reaches this address" against an ANALYZED program.

Run BY GHIDRA, inside Ghidra's own interpreter, loaded by path with no package context, so like
``postscript.py`` it imports nothing at module level and can be imported by a plain CPython test.

It runs on BOTH paths, and that is the point of it:

  * on a cold project (``-import``, analysis on) it answers queries about a program that has just
    been analyzed;
  * on a warm project (``-process -noanalysis``) it answers the same queries against the analysis a
    previous run paid for, which is what makes a second question cost seconds instead of an hour.

WHAT IT CAN AND CANNOT ANSWER, stated here because the numbers it prints look like runtime facts and
are not:

  * it reports references the ANALYZER holds in an imported Raw Binary program: direct `jal` call
    sites, and the data references the MIPS analyzer derived from loads and stores. It cannot see a
    call made through a register whose value it has never seen, a table it did not decode, or
    anything that only happens at run time;
  * ``callers`` is the CODE half of the reference set and ``refs`` is the whole set, with each site's
    access (read/write/control) and the function that contains it. Both carry the count they were
    drawn from, so "0" is a statement about a scanned program rather than about the address alone;
  * an address OUTSIDE the program's memory is answered as ``in_memory: false`` with the range
    printed. It is NOT answered as "0 references", because no reference to it could exist and the two
    statements are not the same claim.

ARGUMENTS::

    <queries.json> <queries-file>

``<queries-file>`` holds one ``kind address`` question per line; ``<queries.json>`` receives the
answers. The parser is the MIRROR of :func:`tools.decomp.queries.parse_query_text`, pinned by test --
this file cannot import a repository module at run time, so the mirror is the floor imposed by the
host boundary and the pin is what makes it safe.
"""

from __future__ import annotations

import json
import os

CALLERS = "callers"
REFS = "refs"
FUNCTION_AT = "function_at"
KINDS = (CALLERS, REFS, FUNCTION_AT)
ALIASES = {"function-at": FUNCTION_AT, "function_at": FUNCTION_AT,
           "callers": CALLERS, "refs": REFS}

# Ghidra's own FlowType names, lowercased, that mean "this site READS the address" and "this site
# WRITES it". Taken from the reference's own flow type when Ghidra offers one (the primary source,
# because Ghidra is the authority on its own analysis) and only then from the mnemonic.
READ_FLOWS = frozenset({"read", "read_write"})
WRITE_FLOWS = frozenset({"write", "read_write"})

# The fallback classification, and it is a FALLBACK for a stated reason: a Ghidra version that does
# not expose a flow type on an instruction would otherwise leave every access as "unknown", and
# "unknown" is a worse answer than a rule an agent can check. MIPS-I stores are the five below; a
# store that is not in this set is reported as `store` rather than as a read.
STORE_MNEMONICS = frozenset({"sw", "sb", "sh", "sd", "sdc1", "swc1", "sdl"})


class QueryScriptRefusal(Exception):
    """The query file or the document path could not be used. Raised so Ghidra reports it."""


def parse_query_text(text, origin="<queries>"):
    """``kind address`` per line, ``#`` to end of line. MIRROR of queries.parse_query_text."""
    body = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    queries = []
    seen = set()
    for number, raw in enumerate(body.splitlines(), start=1):
        fields = raw.replace(",", " ").split()
        if not fields:
            continue
        if len(fields) != 2:
            raise QueryScriptRefusal(
                "%s line %d: expected `kind address`, got %r" % (origin, number, raw.strip()))
        kind = ALIASES.get(fields[0].lower(), fields[0].lower())
        if kind not in KINDS:
            raise QueryScriptRefusal(
                "%s line %d: %r is not one of %s" % (origin, number, kind, ", ".join(KINDS)))
        try:
            address = int(fields[1], 16)
        except ValueError as error:
            raise QueryScriptRefusal(
                "%s line %d: %r is not a hex guest address" % (origin, number, fields[1])) from error
        if not 0 <= address <= 0xFFFFFFFF:
            raise QueryScriptRefusal("%s line %d: 0x%X is not a 32-bit address"
                                     % (origin, number, address))
        if (kind, address) in seen:
            continue
        seen.add((kind, address))
        queries.append((kind, address))
    if not queries:
        raise QueryScriptRefusal("%s holds no questions." % origin)
    return queries


def read_queries(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    except OSError as error:
        raise QueryScriptRefusal("cannot read the query list at %s: %s" % (path, error))
    return parse_query_text(raw, origin=str(path))


def access_of(instruction):
    """``(access, source)`` for a site. ``source`` says WHICH rule decided, every time."""
    if instruction is None:
        return "unknown", "no instruction at the site"
    mnemonic = instruction.getMnemonicString()
    flow = None
    try:
        value = instruction.getFlowType()
    except Exception:  # noqa: BLE001 - an older Ghidra simply does not offer it
        value = None
    if value is not None:
        flow = str(value).lower()
    if flow:
        # Ghidra's own enum names for control transfers include UNCONDITIONAL_CALL, UNCONDITIONAL_JUMP,
        # CONDITIONAL_JUMP and CALL_TERMINATOR, so the test is on the family rather than on four
        # literals that one Ghidra release would rename. A site printed as `other` for being a CALL
        # would be a confident wrong answer about what a reference does.
        if "call" in flow or "jump" in flow or "branch" in flow:
            return "control", "ghidra flow type %s" % flow
        if flow in WRITE_FLOWS and flow in READ_FLOWS:
            return "read/write", "ghidra flow type %s" % flow
        if flow in WRITE_FLOWS:
            return "write", "ghidra flow type %s" % flow
        if flow in READ_FLOWS:
            return "read", "ghidra flow type %s" % flow
        return "other", "ghidra flow type %s" % flow
    if mnemonic in STORE_MNEMONICS:
        return "write", "mnemonic %s (no flow type offered)" % mnemonic
    return "read", "mnemonic %s (no flow type offered)" % mnemonic


def _reference_type_name(reference):
    try:
        return str(reference.getReferenceType()).upper()
    except Exception:  # noqa: BLE001 - a name is a convenience, never a count
        return "UNKNOWN"


def _is_code_reference(type_name):
    return ("CALL" in type_name) or type_name in ("FLOW", "UNCONDITIONAL_JUMP", "CONDITIONAL_JUMP")


def main(argv=None):
    if argv is None:
        argv = getScriptArgs()  # noqa: F821 - provided by GhidraScript
    if len(argv) < 2:
        print("REFUSED: this query script needs <queries.json> <queries-file>, got %r" % (argv,))
        return 2
    document_path, queries_path = argv[0], argv[1]
    questions = read_queries(queries_path)

    program = currentProgram  # noqa: F821 - provided by GhidraScript
    functions = program.getFunctionManager()
    listing = program.getListing()
    references = program.getReferenceManager()
    memory = program.getMemory()
    factory = program.getAddressFactory()

    def address_of(value):
        # NOT toAddr(value): these are KSEG0 addresses (>= 0x80000000) and the int overload
        # overflows Java's signed int. Same rule, same reason, as the sibling post-script.
        return factory.getAddress("%08x" % value)

    def containing_function(offset):
        function = functions.getFunctionContaining(address_of(offset))
        if function is None:
            return None
        return function

    def describe(function):
        if function is None:
            return "?", "-"
        return ("%08X" % function.getEntryPoint().getOffset(), function.getName())

    def in_memory(offset):
        block = memory.getBlock(address_of(offset))
        return block is not None

    def bounded_instructions(function):
        total = 0
        iterator = listing.getInstructions(function.getBody(), True)
        while iterator.hasNext():
            iterator.next()
            total += 1
        return total

    memory_first = memory.getMinAddress().getOffset()
    memory_last = memory.getMaxAddress().getOffset()
    function_count = len(list(functions.getFunctions(True)))

    answers = []
    for kind, offset in questions:
        address = address_of(offset)
        present = in_memory(offset)
        entry = {
            "mode": kind,
            "address": "0x%08X" % offset,
            "in_memory": bool(present),
            "sites": [],
        }
        if not present:
            # NOT a zero. The answer names the fact that decides it, so the zero cannot be read as
            # "this address has no referrers" -- it is outside the program that was scanned.
            entry["note"] = ("0x%08X is outside this program's memory (0x%08X..0x%08X), so no "
                             "reference to it can exist." % (offset, memory_first, memory_last))
            # The COUNT keys are present with zero even though the answer is not a count: they are
            # what the host-side loader checks the listed sites against, and a document missing
            # them is refused rather than read as "an answer with nothing in it".
            entry["references_found"] = 0
            entry["code_references"] = 0
            entry["data_references"] = 0
            entry["functions_touched"] = 0
            entry["unattributed_sites"] = 0
            entry["instruction_count"] = 0
            entry["function_found"] = False
            answers.append(entry)
            continue

        if kind == FUNCTION_AT:
            function = functions.getFunctionAt(address)
            owner = None
            if function is None:
                owner = containing_function(offset)
            body_owner = owner if owner is not None else function
            if body_owner is None:
                entry.update({
                    "function_found": False,
                    "instruction_count": 0,
                    "is_entry": False,
                    "note": ("no function is defined at this address and it is not inside another "
                             "function's body either. Auto-analysis only defines a function where "
                             "it saw a call or branch, so a jump-table-only entry looks exactly "
                             "like this; the query does not establish which."),
                })
            else:
                body = body_owner.getBody()
                entry.update({
                    "function_found": True,
                    "entry": "0x%08X" % body_owner.getEntryPoint().getOffset(),
                    "name": body_owner.getName(),
                    "body_first": "0x%08X" % body.getMinAddress().getOffset(),
                    "body_last": "0x%08X" % body.getMaxAddress().getOffset(),
                    "instruction_count": bounded_instructions(body_owner),
                    "is_entry": bool(function is not None),
                    "offset_into_body": offset - body.getMinAddress().getOffset(),
                })
                if function is None:
                    entry["note"] = (
                        "this address is a label INSIDE %s's body, not a function entry; the answer "
                        "names the enclosing function." % body_owner.getName())
            answers.append(entry)
            continue

        wanted_code_only = kind == CALLERS
        iterator = references.getReferencesTo(address)
        sites = []
        code_count = 0
        data_count = 0
        functions_seen = set()
        unattributed = 0
        while iterator.hasNext():
            reference = iterator.next()
            from_address = reference.getFromAddress()
            source_offset = from_address.getOffset()
            type_name = _reference_type_name(reference)
            code_reference = _is_code_reference(type_name)
            if wanted_code_only and not code_reference:
                # COUNTED, not dropped: a caller list that silently discards the data references it
                # saw cannot be checked against a data list built by --refs.
                data_count += 1
                continue
            if code_reference:
                code_count += 1
            else:
                data_count += 1
            owner = containing_function(source_offset)
            if owner is None:
                unattributed += 1
                owner_entry, owner_name = "?", "-"
            else:
                owner_entry, owner_name = describe(owner)
                functions_seen.add(owner_entry)
            instruction = listing.getInstructionAt(from_address)
            access, access_source = access_of(instruction)
            sites.append({
                "from": "0x%08X" % source_offset,
                "type": type_name,
                "access": access,
                "access_source": access_source,
                "function": owner_entry,
                "function_name": owner_name,
                "text": (instruction.toString() if instruction is not None else "<no instruction>"),
            })
        sites.sort(key=lambda item: int(item["from"], 16))
        entry.update({
            "references_found": code_count if wanted_code_only else code_count + data_count,
            "code_references": code_count,
            # For `--callers` the data references are EXCLUDED from the answer, so they are not
            # counted as part of it; they are reported separately as seen-but-not-listed. Counting
            # them in both places would make `found = code + data` false for the same address
            # depending on which question was asked, which is the kind of number that only holds
            # for one caller.
            "data_references": 0 if wanted_code_only else data_count,
            "data_references_seen": data_count,
            "functions_touched": len(functions_seen),
            "unattributed_sites": unattributed,
            "sites": sites,
        })
        if wanted_code_only:
            entry["note"] = ("callers answers with the CODE references only; %d data reference(s) "
                             "to the same address were seen and are listed by --refs" % data_count)
        answers.append(entry)

    document = {
        "program": program.getName(),
        "image_name": os.environ.get("PSXPORT_DECOMP_IMAGE_NAME", ""),
        "image_kind": os.environ.get("PSXPORT_DECOMP_IMAGE_KIND", ""),
        "language": str(program.getLanguageID()),
        "memory_first": "0x%08X" % memory_first,
        "memory_last": "0x%08X" % memory_last,
        "functions_scanned": function_count,
        "queries_requested": len(questions),
        "queries": answers,
    }
    with open(document_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("QUERYSCRIPT-OK queries=%d functions=%d memory=0x%08X..0x%08X"
          % (len(answers), function_count, memory_first, memory_last))
    return 0


# The entry point, for the reason the sibling scripts document at length: PyGhidra loads a script as
# the module named '__main__', so `__name__` is the only reliable discriminator, and a script with a
# `main()` and no call to it logs "Post-analysis succeeded" and writes nothing.
if __name__ == "__main__":
    main()