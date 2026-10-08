"""Ghidra PRE-script for the no-return probe: SEED ONLY, and report the analyzer's own state.

Three MEASURED reasons this is shaped the way it is, each of which produced a clean-looking zero
before it was fixed:

  1. A linear sweep from the fixture's first byte decodes 2 instructions and finds 0 functions -- the
     fixture opens with an infinite branch loop the sweep enters and never leaves. Sweeping the
     window is the WRONG instrument for a fixture that contains a loop at all.
  2. `createFunction` at every entry in the PRE-script leaves auto-analysis nothing to infer, and the
     non-return analyzer is an INFERENCE over call sites. Pre-creating the functions is very
     plausibly why it reported 0 -- so this seeds ONE entry and lets the analysis find the rest.
  3. "It marked 0" is only meaningful beside "it ran". So this asks the analyzer itself: which
     non-return options exist, and are they enabled. A zero from a disabled analyzer and a zero from
     an enabled one that declined to fire are different facts, and only the first is a clean result.
"""
import json
import os

MARKER = os.environ["PSXPORT_NORETURN_PROBE"]


def main():
    factory = currentProgram.getAddressFactory()
    fm = currentProgram.getFunctionManager()

    # "It marked 0" is only meaningful beside "it ran", so this asks the analyzer itself. The method
    # is `getAnalysisOption`/`setAnalysisOptions` -- there is NO `getAnalysisOptions`, and asking for
    # it raises NameError and leaves the whole pre-script unrun while Ghidra continues and the run
    # reports success. MEASURED here, and the same trap the spyro pre-script recorded.
    nonreturn = {}
    for name in ("Non-Returning Functions - Discovered",
                 "Non-Returning Functions - Aggressive"):
        try:
            nonreturn[name] = str(getAnalysisOption(name))
        except Exception as error:  # noqa: BLE001 - an unreadable option is still a fact
            nonreturn[name] = "unreadable: %r" % (error,)

    with open(MARKER.replace(".json", "_targets.json")) as handle:
        wanted = [int(t, 16) for t in handle.read().split()]

    # Seed the spinner (whose branch loop the sweep would otherwise never leave), then CREATE every
    # other entry. Both are needed and the reason is measured: seeding only the spinner leaves 0
    # functions, so the post-script decompiles nothing and every row reads 0 bytes -- which looks
    # like "the failure did not reproduce" and is actually "there was nothing to decompile".
    seeded = 1 if disassemble(factory.getAddress("%08x" % wanted[0])) else 0
    created = 0
    for address in wanted[1:]:
        at = factory.getAddress("%08x" % address)
        if fm.getFunctionAt(at) is None:
            created += 1 if createFunction(at, None) is not None else 0

    marked = []
    for fn in fm.getFunctions(True):
        if fn.hasNoReturn():
            marked.append("%08X" % fn.getEntryPoint().getOffset())

    with open(MARKER, "w") as handle:
        json.dump({"seeded": seeded, "created_on_demand": created,
                   "functions_after_seed": fm.getFunctionCount(),
                   "noreturn_options": nonreturn, "noreturn_marked": sorted(marked),
                   "targets": ["0x%08X" % t for t in wanted]}, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("NORETURN-PROBE seeded=%d created=%d functions=%d marked=%d"
          % (seeded, created, fm.getFunctionCount(), len(marked)))


main()
