# Ghidra headless post-script: decompile functions to C.
# Runs under Ghidra's Jython. Invoked by tools/decomp.sh.
#
# Script args: <out.c> [lo_hex hi_hex]
#   out.c     - output file (combined C for all decompiled functions)
#   lo,hi     - optional address window; only functions with entry in [lo,hi)
#
# The RAM dump is imported as a raw binary based at 0x80000000 (KSEG0), so all
# addresses match the virtual addresses used throughout docs/journal.md.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
outpath = args[0] if len(args) > 0 else "decomp_all.c"
# Two selection modes after <out.c>:
#   <lo_hex> <hi_hex>          -> functions whose entry is in [lo,hi)   (range)
#   list <addr_hex> [addr...]  -> exactly these function entries        (explicit list)
# pyghidraRun -H (Ghidra 12) forwards -postScript args as a SINGLE space-joined
# string, so "list a b c" arrives as ["list a b c"]. Flatten on whitespace so both
# the per-arg (analyzeHeadless) and joined-arg (pyghidraRun) forms parse identically.
rest = [tok for a in args[1:] for tok in str(a).split()]
lo = hi = None
want = None
if len(rest) >= 1 and rest[0] == "list":
    want = set(int(a, 16) for a in rest[1:])
elif len(rest) >= 2:
    lo = int(rest[0], 16)
    hi = int(rest[1], 16)

decomp = DecompInterface()
decomp.toggleCCode(True)
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()

def emit(f, fn):
    ea = fn.getEntryPoint().getOffset()
    res = decomp.decompileFunction(fn, 90, monitor)
    f.write("// ==================== %08X %s ====================\n" % (ea, fn.getName()))
    if res is not None and res.decompileCompleted():
        f.write(res.getDecompiledFunction().getC())
    else:
        f.write("// DECOMPILE FAILED: %s\n" % (res.getErrorMessage() if res else "no result"))
    f.write("\n")


count = 0
created = []
missing = []
f = open(outpath, "w")
if want is not None:
    # EXPLICIT LIST: address-driven, not "iterate what Ghidra happens to have". Auto-analysis only
    # defines a function where it found a call/branch to it, so a perfectly valid entry reached only
    # by a jump TABLE (which is most render/dispatch handlers here) has no Function object and used
    # to be skipped in silence — the script reported "wrote 0 functions" and named nothing. Create the
    # function on demand, and if that still fails, SAY WHICH ADDRESS rather than dropping it.
    for ea in sorted(want):
        # NOT toAddr(ea): these are KSEG0 addresses (>= 0x80000000) and the int overload overflows
        # Java's signed int ("Cannot convert value to Java int"). Go through the address factory
        # with a hex STRING, which is width-agnostic.
        addr = currentProgram.getAddressFactory().getAddress("%08x" % ea)
        fn = fm.getFunctionAt(addr)
        if fn is None:
            fn = createFunction(addr, None)          # disassembles + defines a body if it can
            if fn is not None:
                created.append(ea)
        if fn is None:
            missing.append(ea)
            f.write("// ==================== %08X ====================\n" % ea)
            f.write("// NO FUNCTION at this address (could not create one; is it code in this dump?)\n\n")
            continue
        emit(f, fn)
        count += 1
else:
    for fn in fm.getFunctions(True):
        ea = fn.getEntryPoint().getOffset()
        if lo is not None and not (lo <= ea < hi):
            continue
        emit(f, fn)
        count += 1
f.close()
print("[ghidra_decomp] wrote %d functions -> %s" % (count, outpath))
if created:
    print("[ghidra_decomp] created %d function(s) not found by auto-analysis: %s"
          % (len(created), " ".join("%08X" % a for a in created)))
if missing:
    print("[ghidra_decomp] MISSING %d requested address(es): %s"
          % (len(missing), " ".join("%08X" % a for a in missing)))
