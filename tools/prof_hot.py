#!/usr/bin/env python3
"""Resolve a PSXPORT_PROF host-PC sample file into a per-symbol profile.

hostprof.cpp's own header has promised this tool since it was written ("tools/prof_hot.py does the
symbol mapping with nm, which keeps this file free of any symbol-table parsing"). It did not exist.
Neither did a working profiler: hostprof_init() was compiled but CALLED FROM NOWHERE, so
`PSXPORT_PROF=1` came back from the exit audit as "set for this whole run and NOTHING ever read it".
Both are fixed together, because a profiler with no reader and a reader with no profiler are the same
amount of evidence: none.

Do not confuse this with tools/prof_report.py, which reads the INTERPRETER profiler's dump and works
in GUEST addresses. This one works in HOST addresses and needs the binary that produced the samples.

    tools/prof_hot.py scratch/raw/prof_host.txt scratch/bin/tomba2_port [--top N]

UNRESOLVED SAMPLES ARE REPORTED, NOT HIDDEN. A host PC can land outside every text symbol nm knows
about (PLT stubs, a stripped vendored object, JIT-less trampolines). Folding those into "the nearest
preceding symbol" is how a 20% bucket ends up attributed to `data_start` and read as a hot function.
They get their own line, with the count, so a profile that resolves badly says so.
"""
import argparse
import bisect
import collections
import subprocess
import sys
from pathlib import Path


def load_symbols(binary):
    """Text symbols, sorted by address, with the END of the text region for range checking."""
    out = subprocess.run(["nm", "-C", "--defined-only", "-S", str(binary)],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 3:
            continue
        # with -S: "addr size type name"; without a size: "addr type name"
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if len(parts) >= 4 and parts[2] in "tTwW":
            syms.append((addr, int(parts[1], 16), parts[3]))
        elif parts[1] in "tTwW":
            syms.append((addr, 0, parts[2]))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("samples", help="PSXPORT_PROF_OUT file (default scratch/raw/prof_host.txt)")
    ap.add_argument("binary", help="the executable that produced them — symbols must match")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    for p in (args.samples, args.binary):
        if not Path(p).is_file():
            print(f"REFUSED: {p} does not exist", file=sys.stderr)
            return 2

    syms = load_symbols(args.binary)
    if not syms:
        print(f"REFUSED: nm found no text symbols in {args.binary} — a stripped binary cannot be "
              f"profiled by symbol", file=sys.stderr)
        return 2
    addrs = [a for a, _, _ in syms]

    total = 0
    hits = collections.Counter()
    unresolved = 0
    for line in open(args.samples):
        if line.startswith("#") or not line.strip():
            continue
        a_s, n_s = line.split()
        addr, n = int(a_s, 16), int(n_s)
        total += n
        i = bisect.bisect_right(addrs, addr) - 1
        if i < 0:
            unresolved += n
            continue
        start, size, name = syms[i]
        # A symbol with a known size bounds its own range; without one, accept only a modest gap so a
        # PC far past the last symbol is not silently credited to it.
        if (size and addr >= start + size) or (not size and addr - start > 0x10000):
            unresolved += n
            continue
        hits[name] += n

    if total == 0:
        print("REFUSED: the sample file holds no samples — nothing to report", file=sys.stderr)
        return 2

    resolved = total - unresolved
    print(f"# {args.samples}: {total} samples, {resolved} resolved ({100*resolved/total:.1f}%), "
          f"{unresolved} unresolved ({100*unresolved/total:.1f}%) over {len(syms)} text symbols")
    if unresolved and unresolved / total > 0.05:
        print(f"# NOTE: {100*unresolved/total:.1f}% of samples fell outside every known text symbol. "
              f"Percentages below are of the TOTAL, so they still sum honestly, but a large unresolved "
              f"share means the ranking is incomplete — not that the named functions are cheap.")
    print(f"\n{'%tot':>6}  {'samples':>8}  symbol")
    for name, n in hits.most_common(args.top):
        print(f"{100*n/total:6.2f}  {n:8d}  {name}")
    if unresolved:
        print(f"{100*unresolved/total:6.2f}  {unresolved:8d}  <unresolved — outside every text symbol>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
