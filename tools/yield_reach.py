#!/usr/bin/env python3
"""yield_reach.py — does a function transitively reach the cooperative YIELD (FUN_80051f80)?

Scans a raw 2MB KSEG0 RAM dump, follows `jal` targets recursively from a start address,
and reports whether the yield (default 0x80051f80) is reachable. Used to decide whether a
DEMO/GAME substate handler callee can be plain rec_dispatch'd from inside a native override
(SYNCHRONOUS => safe) or must run IN-CONTEXT via the coro-redirect handshake (YIELDS => a
deep longjmp would destroy the override's C frame and kill the task).

LIMITATION: only follows DIRECT `jal` (op 0x03). Indirect/computed calls (`jalr`, `jr v0`
switch tables) are NOT followed — so a "no yield found" result is a LOWER BOUND. It prints
every indirect-call site reached so you know where the blind spots are.

Usage: tools/yield_reach.py 0x80052078 [more addrs...] --ram scratch/bin/tomba2/ram_menu.bin
       [--yield 0x80051f80] [--max 4000]
"""
import sys, struct

def main():
    args = sys.argv[1:]
    ram_path = "scratch/bin/tomba2/ram_menu.bin"
    yield_addr = 0x80051f80
    maxfns = 4000
    starts = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--ram": ram_path = args[i+1]; i += 2
        elif a == "--yield": yield_addr = int(args[i+1], 0); i += 2
        elif a == "--max": maxfns = int(args[i+1], 0); i += 2
        else: starts.append(int(a, 0)); i += 1
    if not starts:
        print(__doc__); return 1

    with open(ram_path, "rb") as f:
        ram = f.read()
    BASE = 0x80000000
    def r32(a):
        o = (a & 0x1fffff)
        if o + 4 > len(ram): return 0
        return struct.unpack_from("<I", ram, o)[0]

    for start in starts:
        seen = set()
        stack = [start]
        reaches = False
        indirect = []
        n = 0
        while stack and n < maxfns:
            fn = stack.pop()
            if fn in seen: continue
            seen.add(fn); n += 1
            # walk the function until `jr ra` (end). Cap at 0x2000 bytes per fn as a safety.
            a = fn
            end = fn + 0x2000
            while a < end:
                ins = r32(a)
                op = ins >> 26
                if op == 0x03:  # jal
                    tgt = ((ins & 0x3ffffff) << 2) | (a & 0xf0000000)
                    if tgt == yield_addr:
                        reaches = True
                    elif tgt not in seen and (tgt & 0xfffffff) < 0x200000 + 0x100000:
                        stack.append(tgt)
                elif op == 0x00:
                    fn6 = ins & 0x3f
                    if fn6 == 0x09:  # jalr (indirect call)
                        indirect.append(a)
                    if fn6 == 0x08 and ((ins >> 21) & 0x1f) == 31:  # jr ra -> end of fn
                        # account for delay slot then stop scanning this fn
                        nxt = r32(a + 4)
                        if (nxt >> 26) == 0x03:
                            t2 = ((nxt & 0x3ffffff) << 2) | ((a+4) & 0xf0000000)
                            if t2 == yield_addr: reaches = True
                            elif t2 not in seen: stack.append(t2)
                        break
                a += 4
        tag = "YIELDS (needs coro-redirect)" if reaches else "no direct yield (likely SYNC)"
        print(f"0x{start:08x}: {tag}  [{n} fns scanned, {len(indirect)} indirect-call sites]")
        if indirect and not reaches:
            shown = indirect[:8]
            print("    indirect calls (blind spots): " + " ".join(f"0x{x:08x}" for x in shown)
                  + (" ..." if len(indirect) > 8 else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main())
