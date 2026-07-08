# symwidth_re.py — Ghidra headless post-script: print Ghidra's DECLARED DATA WIDTH at given addresses.
#
# WHY: Ghidra's decompiled C often names a bare global access `DAT_<addr>` with no visible cast
# (e.g. `DAT_800bf874 = DAT_800bf874 + 100;` reads as a generic 32-bit op even when the real access
# is a byte). Getting the width wrong when translating to native C++ silently clobbers neighboring
# guest bytes — invisible until an SBS byte-compare catches it (or doesn't, if nothing else reads
# that memory). This asks Ghidra's own type database for the size it resolved at each address,
# instead of inferring width from a linear disassembly scan (which can misattribute a stale
# register value across unrelated code regions in a switch-heavy function — see the psxport
# ActorReward session, docs/findings/tooling.md, for a case where that happened).
#
# USAGE (see tools/decomp.sh for the pyghidraRun invocation shape):
#   DW_ADDRS="800bf874,800e7fee,1f80017c" \
#   pyghidraRun -H scratch/ghidra <project> -process -noanalysis \
#     -scriptPath tools -postScript symwidth_re.py -scriptlog scratch/logs/symwidth.log \
#     2>&1 | grep '^WIDTH'
#
# A result of "UNDEFINED" means Ghidra never created typed Data there (common for scratchpad
# addresses computed via a register-relative load Ghidra didn't propagate into a Data creation) —
# fall back to reading the raw opcode's load/store width directly (tools/disas.py --mem) in that case.
import os
addrs = os.environ.get("DW_ADDRS", "").split(",")
listing = currentProgram.getListing()
af = currentProgram.getAddressFactory().getDefaultAddressSpace()
for a in addrs:
    a = a.strip()
    if not a: continue
    addr = af.getAddress(int(a, 16))
    data = listing.getDataAt(addr)
    if data is not None:
        print("WIDTH %s = %s (len=%d)" % (a, data.getDataType().getName(), data.getLength()))
    else:
        # maybe it's mid-instruction referenced as scalar only, check defined data before/at
        u = listing.getDefinedDataContaining(addr)
        if u is not None:
            print("WIDTH %s ~ %s (len=%d, base=%s)" % (a, u.getDataType().getName(), u.getLength(), u.getAddress()))
        else:
            print("WIDTH %s = UNDEFINED" % a)
