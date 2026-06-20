#!/usr/bin/env python3
# Aggregate a `prof dump` file (from the in-port REPL profiler, interp.cpp) into a per-FUNCTION
# perf report, naming each hot PC bucket / call target by the enclosing resident function.
#
# The port has NO recompiler-time profiler — every un-owned engine fn + all game CONTENT runs through
# the flat MIPS interpreter, so this is the tool that answers "which engine fn should I own next for
# perf?" (the #1 priority: owning a hot fn natively advances 100%-PC-native AND speeds the game up).
#
# Usage:  tools/prof_report.py scratch/raw/prof.txt [--top N]
#   The dump's TIME section (PC buckets) is aggregated by enclosing function (from
#   tools/recomp/tomba2_funcs.txt) and re-sorted, so a function spread over many buckets sums up.
#   The FREQUENCY section (call targets) is named in place.
#   Overlay (DEMO/GAME.BIN) addresses aren't in the resident func list -> shown as overlay:<addr>.
import sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
FUNCS = os.path.join(HERE, "recomp", "tomba2_funcs.txt")

def load_funcs():
    addrs = []
    with open(FUNCS) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                addrs.append(int(ln, 16))
    addrs.sort()
    return addrs

def enclosing(addrs, a):
    # largest func addr <= a (binary search)
    lo, hi = 0, len(addrs)
    while lo < hi:
        mid = (lo + hi) // 2
        if addrs[mid] <= a: lo = mid + 1
        else: hi = mid
    if lo == 0: return None
    return addrs[lo - 1]

def name(addrs, a):
    f = enclosing(addrs, a)
    # resident MAIN.EXE roughly 0x80010000..0x800a0000; overlays load above/around that range too.
    if f is None: return f"overlay:{a:08X}"
    off = a - f
    # if the bucket is implausibly far past the function start (> 0x4000), it's likely overlay code
    # at an address that happens to sit after the last resident func -> mark it.
    if off > 0x8000: return f"overlay:{a:08X}"
    return f"FUN_{f:08X}+0x{off:X}" if off else f"FUN_{f:08X}"

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 30
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
    if not args:
        print("usage: prof_report.py <prof_dump.txt> [--top N]"); sys.exit(1)
    addrs = load_funcs()
    section = None
    time_rows = []     # (addr, insns)
    freq = []          # (addr, calls)
    header = ""
    with open(args[0]) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if ln.startswith("# prof:"): header = ln[2:]; continue
            if "TIME" in ln: section = "time"; continue
            if "FREQUENCY" in ln: section = "freq"; continue
            if ln.startswith("#") or not ln.strip(): continue
            parts = ln.split()
            if section == "time" and len(parts) >= 2:
                time_rows.append((int(parts[0], 16), int(parts[1])))
            elif section == "freq" and len(parts) >= 2:
                freq.append((int(parts[0], 16), int(parts[1])))

    # OVERLAY RESOLUTION: tomba2_funcs.txt only lists resident + a sparse set of overlay functions, so
    # several real overlay functions get merged under one label (e.g. the pure leaf 0x8013fae0 was lumped
    # into "FUN_8013F0DC+0xA04"). The FREQUENCY section lists genuine CALL TARGETS (jal/jalr entry points
    # logged by the interpreter), so merging those addresses into the boundary set splits the merged TIME
    # buckets at real function starts. This makes the overlay hot-list trustworthy (later-188).
    boundaries = sorted(set(addrs) | {a for a, _ in freq})
    time_by_fn = {}    # func_start -> insns
    for a, n in time_rows:
        f0 = enclosing(boundaries, a)
        key = f0 if (f0 is not None and a - f0 <= 0x8000) else a
        time_by_fn[key] = time_by_fn.get(key, 0) + n

    resident = set(addrs)
    def label(k):
        # k is a resolved function start (from funcs.txt or a real call target). Mark overlay starts
        # that funcs.txt doesn't list so it's clear the symbol came from call-target resolution.
        if k in resident: return f"FUN_{k:08X}"
        f0 = enclosing(addrs, k)
        if f0 is not None and k - f0 <= 0x8000 and k != f0: return f"ov_{k:08X}(>{f0:08X})"
        return f"ov_{k:08X}"

    total_time = sum(time_by_fn.values())
    print(f"=== {header} ===\n")
    print(f"--- TIME by function (top {top}; % of {total_time} sampled insns; ov_=overlay call-target) ---")
    print(f"{'function':<28} {'insns':>12} {'pct':>7}")
    for k, n in sorted(time_by_fn.items(), key=lambda kv: -kv[1])[:top]:
        pct = 100.0 * n / total_time if total_time else 0
        print(f"{label(k):<28} {n:>12} {pct:>6.2f}%")

    total_freq = sum(n for _, n in freq)
    print(f"\n--- FREQUENCY: most-called interpreted (un-owned) functions (top {top}) ---")
    print(f"{'function':<28} {'calls':>12} {'pct':>7}")
    for a, n in sorted(freq, key=lambda x: -x[1])[:top]:
        pct = 100.0 * n / total_freq if total_freq else 0
        print(f"{label(a):<28} {n:>12} {pct:>6.2f}%")

if __name__ == "__main__":
    main()
