#!/usr/bin/env python3
# Diff two 2 MB guest-RAM dumps (PSXPORT_RAMDUMP) and bucket the differing bytes by memory region.
# Purpose: isolate whether a change (e.g. the widescreen cull re-include) touches GAMEPLAY state or only
# the render buffers. Render-buffer regions are from docs/engine_re.md "Render-buffer memory map" (later-125).
# Usage: tools/ram_region_diff.py A.bin B.bin
import sys

# (phys_lo, phys_hi, name) — phys = addr & 0x1FFFFF (the dump is main RAM 0x80000000..0x80200000).
REGIONS = [
    (0x0BFE68, 0x0D3E68, "packet-pool parity0 (render)"),
    (0x0D3E68, 0x0E7E68, "packet-pool parity1 (render)"),
    (0x0E7E68, 0x0E80A8, "pool/ctx gap globals"),
    (0x0E80A8, 0x0EA0A8, "OT array parity0 (render)"),
    (0x0EA0A8, 0x0EA118, "DISP/DRAW env parity0 (render)"),
    (0x0EA118, 0x0EC118, "OT array parity1 (render)"),
    (0x0EC118, 0x0EC188, "DISP/DRAW env parity1 (render)"),
]

def region_of(off):
    for lo, hi, name in REGIONS:
        if lo <= off < hi:
            return name
    return "OTHER (gameplay/engine state)"

def main():
    if len(sys.argv) != 3:
        print("usage: ram_region_diff.py A.bin B.bin"); sys.exit(2)
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    n = min(len(a), len(b))
    if len(a) != len(b):
        print(f"WARN: size mismatch {len(a)} vs {len(b)}; comparing first {n}")
    counts, ranges = {}, []   # per-region differing-byte count; contiguous differing ranges
    cur_lo = None
    for i in range(n):
        if a[i] != b[i]:
            r = region_of(i); counts[r] = counts.get(r, 0) + 1
            if cur_lo is None: cur_lo = i
            cur_last = i
        else:
            if cur_lo is not None:
                ranges.append((cur_lo, cur_last)); cur_lo = None
    if cur_lo is not None: ranges.append((cur_lo, cur_last))

    total = sum(counts.values())
    print(f"differing bytes: {total} / {n}  ({100.0*total/n:.4f}%)")
    print("--- by region ---")
    for r in sorted(counts, key=lambda k: -counts[k]):
        print(f"  {counts[r]:>8}  {r}")
    # Show the OTHER (gameplay) ranges in full — those are the ones that matter for logic divergence.
    other = [(lo, hi) for (lo, hi) in ranges if region_of(lo) == "OTHER (gameplay/engine state)"]
    print(f"--- OTHER (gameplay) differing ranges: {len(other)} ---")
    for lo, hi in other[:200]:
        print(f"  0x80{lo:06X}..0x80{hi:06X}  ({hi-lo+1} B)")
    if len(other) > 200:
        print(f"  ... and {len(other)-200} more")

if __name__ == "__main__":
    main()
