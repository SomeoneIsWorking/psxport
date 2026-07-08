# symdump_re.py — Ghidra headless post-script: list every DAT_/PTR_ symbol's address.
#
# WHY: Ghidra's decompiled C names an unnamed global `DAT_<addr>` (or `_DAT_<addr>`) — usually the
# same hex as its address, but NOT always (a renamed/aliased symbol breaks that assumption). This
# script prints the ACTUAL address Ghidra resolved for every such symbol in one pass, so a translation
# from decompiled C to native C++ can look addresses up instead of assuming "the name IS the address".
#
# USAGE (see tools/decomp.sh for the pyghidraRun invocation shape):
#   pyghidraRun -H scratch/ghidra <project> -process -noanalysis \
#     -scriptPath tools -postScript symdump_re.py -scriptlog scratch/logs/symdump.log \
#     2>&1 | grep '^SYM'
#
# Pairs with symwidth_re.py (which reports the DECLARED DATA WIDTH at a specific address — the two
# together answer "what global is this DAT_ symbol, and how wide is the access").
st = currentProgram.getSymbolTable()
for sym in st.getAllSymbols(True):
    nm = sym.getName()
    if nm.startswith("DAT_") or nm.startswith("PTR_") or nm.startswith("_DAT_"):
        print("SYM %s = %s" % (nm, sym.getAddress()))
