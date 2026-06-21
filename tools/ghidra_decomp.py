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
rest = args[1:]
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

count = 0
f = open(outpath, "w")
for fn in fm.getFunctions(True):
    ea = fn.getEntryPoint().getOffset()
    if want is not None:
        if ea not in want:
            continue
    elif lo is not None and not (lo <= ea < hi):
        continue
    res = decomp.decompileFunction(fn, 90, monitor)
    f.write("// ==================== %08X %s ====================\n" % (ea, fn.getName()))
    if res is not None and res.decompileCompleted():
        f.write(res.getDecompiledFunction().getC())
    else:
        f.write("// DECOMPILE FAILED: %s\n" % (res.getErrorMessage() if res else "no result"))
    f.write("\n")
    count += 1
f.close()
print("[ghidra_decomp] wrote %d functions -> %s" % (count, outpath))
