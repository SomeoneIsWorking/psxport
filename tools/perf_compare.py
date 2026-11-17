"""Compare the run-end CPU cost lines of two sets of perf_recipe.py logs, phase by phase.

    uv run --frozen python tools/perf_compare.py --a before-1.log before-2.log --b after-1.log after-2.log

For each `[perf] cpu <phase>` line it prints every run's p50 and total, each side's spread (max-min over its runs,
as a share of its mean) and the change of the mean total, plus each side's guest instruction count from the
executor's shutdown ledger. A recipe with a `mark` step (a title whose frame driver marks no perf phases) is
compared by its `[recipe] window cpu` line: process CPU time per field over the marked window. On a host without
an instruction counter the unit is thread CPU time and moves with host load: read the spread before the change,
and treat a change inside it as no change.
"""
from __future__ import annotations

import argparse
import re
import statistics
from pathlib import Path

CPU = re.compile(r"\[perf\] cpu ([\w-]+): p50 ([0-9.]+) p99 ([0-9.]+) max ([0-9.]+) (\S+)/frame, total ([0-9.]+)")
FRAMES = re.compile(r"\[perf\] cpu run-end: (\d+) logic frames metered in (.+?);")
WINDOW = re.compile(r"\[recipe\] window cpu: ([0-9.]+) s over (\d+) fields \(([0-9.]+) cpu-ms/field\)")
GUEST = re.compile(r"shutdown: guest: calls=\d+ executed_blocks=(\d+) executed_instructions=(\d+)")


def parse(path: Path) -> dict:
    run: dict = {"phases": {}, "frames": None, "unit": "?", "guest": None, "window": None}
    for line in path.read_text(errors="replace").splitlines():
        if m := CPU.search(line):
            run["phases"][m.group(1)] = (float(m.group(2)), float(m.group(6)))
            run["unit"] = m.group(5)
        elif m := FRAMES.search(line):
            run["frames"] = int(m.group(1))
        elif m := WINDOW.search(line):
            run["window"] = float(m.group(3))
        elif m := GUEST.search(line):
            run["guest"] = (int(m.group(1)), int(m.group(2)))
    if not run["phases"] and run["window"] is None:
        raise SystemExit(f"{path}: no [perf] cpu lines (was `debug perf` switched on?) and no `mark` window")
    return run


def side(runs: list[dict], phase: str) -> tuple[list[float], list[float]]:
    p50 = [r["phases"][phase][0] for r in runs if phase in r["phases"]]
    per_frame = [r["phases"][phase][1] / r["frames"] for r in runs if phase in r["phases"] and r["frames"]]
    return p50, per_frame


def spread(values: list[float]) -> str:
    if len(values) < 2 or statistics.mean(values) == 0:
        return "  n/a"
    return f"{100 * (max(values) - min(values)) / statistics.mean(values):4.1f}%"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--a", nargs="+", type=Path, required=True, help="the before logs")
    parser.add_argument("--b", nargs="+", type=Path, required=True, help="the after logs")
    args = parser.parse_args()
    a = [parse(p) for p in args.a]
    b = [parse(p) for p in args.b]
    windows = [[r["window"] for r in runs if r["window"] is not None] for runs in (a, b)]
    if all(windows):
        am, bm = statistics.mean(windows[0]), statistics.mean(windows[1])
        print(f"marked window, process CPU (cpu-ms/field): a {' '.join(f'{v:.3f}' for v in windows[0])} "
              f"(spread {spread(windows[0]).strip()}), b {' '.join(f'{v:.3f}' for v in windows[1])} "
              f"(spread {spread(windows[1]).strip()}), change {100 * (bm - am) / am:+.1f}%")
    if any(r["phases"] for r in a + b):
        unit = a[0]["unit"]
        print(f"unit: {unit}/frame; frames metered a={[r['frames'] for r in a]} b={[r['frames'] for r in b]}")
        phases = list(dict.fromkeys(k for r in a + b for k in r["phases"]))
        print(f"{'phase':<12} {'a p50 runs':<22} {'a mean/frame':>12} {'spread':>7}   {'b p50 runs':<22} "
              f"{'b mean/frame':>12} {'spread':>7}  change")
        for phase in phases:
            ap50, aper = side(a, phase)
            bp50, bper = side(b, phase)
            if not aper or not bper:
                continue
            am, bm = statistics.mean(aper), statistics.mean(bper)
            change = f"{100 * (bm - am) / am:+6.1f}%" if am else "   n/a"
            print(f"{phase:<12} {' '.join(f'{v:.3f}' for v in ap50):<22} {am:12.3f} {spread(aper):>7}   "
                  f"{' '.join(f'{v:.3f}' for v in bp50):<22} {bm:12.3f} {spread(bper):>7}  {change}")
    for name, runs in (("a", a), ("b", b)):
        guest = [r["guest"] for r in runs if r["guest"]]
        if guest:
            print(f"{name} guest (whole run): " + ", ".join(f"{blocks} blocks / {insns} instructions"
                                                         for blocks, insns in guest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
