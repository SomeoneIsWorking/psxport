# Ghidra headless post-script: force-disassemble + decompile an address range that
# auto-analysis missed (e.g. a dynamically-loaded overlay reached only via fn ptr).
# Args: <out.c> <lo_hex> <hi_hex> [entry_hex ...]
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.address import AddressSet

args = getScriptArgs()
outpath = args[0]
lo = int(args[1], 16)
hi = int(args[2], 16)
entries = [int(a, 16) for a in args[3:]]

af = currentProgram.getAddressFactory()
def A(off):
    return af.getDefaultAddressSpace().getAddress(off)

# Disassemble the whole window, then create functions at entries (or let analysis find them).
disassemble(A(lo))
for e in entries:
    disassemble(A(e))
    createFunction(A(e), None)

decomp = DecompInterface()
decomp.toggleCCode(True)
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()

f = open(outpath, "w")
n = 0
for fn in fm.getFunctions(A(lo), True):
    ea = fn.getEntryPoint().getOffset()
    if not (lo <= ea < hi):
        break
    res = decomp.decompileFunction(fn, 90, monitor)
    f.write("// ==================== %08X %s ====================\n" % (ea, fn.getName()))
    f.write(res.getDecompiledFunction().getC() if res and res.decompileCompleted()
            else "// FAILED\n")
    f.write("\n")
    n += 1
f.close()
print("[ghidra_overlay] wrote %d functions -> %s" % (n, outpath))
