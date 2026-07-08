# Ghidra headless post-script: list every function that references a given address (read or write).
# Runs under Ghidra's Jython/PyGhidra. Invoked via tools/decomp.sh-style pyghidraRun -H harness.
#
# Script args: <addr_hex>
# Prints, for every reference INTO <addr_hex>, the containing function name+entry and the
# referring instruction address + a short disassembly listing.
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
target_hex = args[0] if len(args) > 0 else "0x800bf839"
target = int(target_hex, 16)

addrFactory = currentProgram.getAddressFactory()
targetAddr = addrFactory.getDefaultAddressSpace().getAddress(target)

refMgr = currentProgram.getReferenceManager()
fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

refs = refMgr.getReferencesTo(targetAddr)
count = 0
for ref in refs:
    fromAddr = ref.getFromAddress()
    fn = fm.getFunctionContaining(fromAddr)
    fnName = fn.getName() if fn else "???"
    fnEntry = fn.getEntryPoint() if fn else None
    instr = listing.getInstructionAt(fromAddr)
    instrText = instr.toString() if instr else "?"
    print("[xref] target=%s from=%s fn=%s@%s instr=%s" % (
        target_hex, fromAddr, fnName, fnEntry, instrText))
    count += 1
print("[xref] total refs to %s: %d" % (target_hex, count))
