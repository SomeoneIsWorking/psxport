"""Ghidra POST-script: does the non-return failure actually reproduce, and does the clear stop it?

THE QUESTION THIS ANSWERS. The pipeline's `PSXPORT_CLEAR_NORETURN=all` plus read-back is the defence
against a caller decompiling to `/* WARNING: Subroutine does not return */` with the body after the
call discarded. On Spyro 1 and on this fixture the analyzer marked **0** functions, so the defence
has never been seen to FIRE. Asserted and unexercised is the state to distrust.

SO THIS SETS THE FLAG ITSELF, on the one function that genuinely never returns, and then measures
BOTH halves of the claim in one run, over the same caller set:

  un-cleared  the caller of that function is decompiled with the flag set. Does its C carry the
              warning, and is the body after the call missing?
  cleared     the same flag is cleared and the SAME caller is decompiled again. Does the body come
              back?

A run that only showed the second half would pass while the first half was broken, which is exactly
the "the check passed but it could not have failed" shape. Both halves, same addresses, one process.
"""
import json
import os

NORETURN_WARNING = "Subroutine does not return"
MARKER = os.environ["PSXPORT_NORETURN_PROBE"]
OUT = os.environ["PSXPORT_NORETURN_PROBE_C"]

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

program = currentProgram
fm = program.getFunctionManager()
af = program.getAddressFactory()
monitor = ConsoleTaskMonitor()
decomp = DecompInterface()
decomp.toggleCCode(True)
decomp.openProgram(program)

with open(MARKER.replace(".json", "_targets.json")) as handle:
    wanted = [int(t, 16) for t in handle.read().split()]

os.makedirs(OUT, exist_ok=True)


def decompile(address):
    fn = fm.getFunctionAt(af.getAddress("%08x" % address))
    if fn is None:
        return None, None
    result = decomp.decompileFunction(fn, 90, monitor)
    if result is None or not result.decompileCompleted():
        return None, None
    return fn, result.getDecompiledFunction().getC()


def statements(text):
    """How many statements the C has. The truncation removes everything after the call, so the
    un-cleared body is expected to be STRICTLY SHORTER. Reported as a number rather than a
    judgement, because 'looks truncated' is not a measurement."""
    if not text:
        return 0
    return sum(1 for line in text.splitlines()
               if line.strip() and not line.strip().startswith(("/*", "*", "}", "{", "#")))


# Which function genuinely never returns, and who calls it. The fixture states this: the spinner at
# noreturn_spin, and its two callers.
SPIN = 0x80010000
CALLERS = [a for a in wanted if a not in (SPIN, 0x80010030)]

rows = {"callers": [], "spin_entry": "0x%08X" % SPIN}

# ---- half 1: UN-CLEARED -----------------------------------------------------------------------
spin_fn = fm.getFunctionAt(af.getAddress("%08x" % SPIN))
if spin_fn is not None:
    spin_fn.setNoReturn(True)
for caller in CALLERS:
    _fn, text = decompile(caller)
    rows["callers"].append({
        "caller": "0x%08X" % caller, "phase": "uncleared",
        "c_bytes": len(text) if text else 0,
        "statements": statements(text),
        "carries_noreturn_warning": bool(text and NORETURN_WARNING in text),
        "calls_spin": bool(text and ("80010000" in text)),
    })
    if text:
        with open(os.path.join(OUT, "uncleared_%08X.c" % caller), "w") as handle:
            handle.write(text)

# ---- half 2: CLEARED, same addresses ----------------------------------------------------------
cleared = 0
still = []
for fn in fm.getFunctions(True):
    if fn.hasNoReturn():
        fn.setNoReturn(False)
        cleared += 1
for fn in fm.getFunctions(True):
    if fn.hasNoReturn():
        still.append("%08X" % fn.getEntryPoint().getOffset())
rows["cleared"] = cleared
rows["still_marked_after_clear"] = sorted(still)
for caller in CALLERS:
    _fn, text = decompile(caller)
    rows["callers"].append({
        "caller": "0x%08X" % caller, "phase": "cleared",
        "c_bytes": len(text) if text else 0,
        "statements": statements(text),
        "carries_noreturn_warning": bool(text and NORETURN_WARNING in text),
        "calls_spin": bool(text and ("80010000" in text)),
    })
    if text:
        with open(os.path.join(OUT, "cleared_%08X.c" % caller), "w") as handle:
            handle.write(text)

decomp.dispose()
with open(MARKER.replace(".json", "_chained.json"), "w") as handle:
    json.dump(rows, handle, indent=2, sort_keys=True)
    handle.write("\n")

uncleared_warned = sum(1 for r in rows["callers"]
                       if r["phase"] == "uncleared" and r["carries_noreturn_warning"])
cleared_warned = sum(1 for r in rows["callers"]
                    if r["phase"] == "cleared" and r["carries_noreturn_warning"])
print("NORETURN-CHAIN set=1 cleared=%d still_marked=%d | un-cleared callers carrying the warning: "
      "%d of %d | cleared: %d of %d"
      % (cleared, len(still), uncleared_warned, len(CALLERS), cleared_warned, len(CALLERS)))
