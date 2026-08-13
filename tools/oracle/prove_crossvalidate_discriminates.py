#!/usr/bin/env python3
"""prove_crossvalidate_discriminates.py — show that crossvalidate_crt0.py compares two REAL sources.

WHY THIS EXISTS
---------------
`crossvalidate_crt0.py` reports agreement between a symbolic decode and a real execution. Agreement is
only meaningful if the two sides could have disagreed. The failure mode that would make its output
worthless is subtle and silent: if a parsing slip made it read both numbers out of the SAME source, it
would report perfect agreement on every game, forever, and look exactly like success.

So this feeds it a MISMATCHED pair — game A's symbolic decode against game B's executed boundary — and
requires every game-specific field to DISAGREE.

The one field expected to match is the BIOS function number (`$t1 = 0x39`, `A(39h) InitHeap`): every
PS-X crt0 makes the same call, so that field genuinely cannot discriminate between games and it is
asserted as SAME rather than quietly tolerated. Naming it explicitly is the point — an unexplained match
here is indistinguishable from the bug this script exists to catch.

Usage:  prove_crossvalidate_discriminates.py <exe-A> <exe-B> [--build DIR]
Exit:   0 = it discriminates. 1 = it does not, and every agreement it has reported is suspect.
        2 = could not run the proof, which is NOT a pass.
"""
import argparse
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def die(msg):
    print(f"prove_crossvalidate_discriminates: REFUSING (exit 2): {msg}")
    print("  Nothing was proven. This is not a pass.")
    sys.exit(2)


def load_cv():
    path = os.path.join(HERE, "crossvalidate_crt0.py")
    if not os.path.exists(path):
        die(f"no crossvalidate_crt0.py at {path}")
    spec = importlib.util.spec_from_file_location("cv", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe_a")
    ap.add_argument("exe_b")
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    ap.add_argument("--steps", type=int, default=900000)
    a = ap.parse_args()

    cv = load_cv()
    extract = os.path.join(a.build, "tools", "crt0_extract")
    tracer = os.path.join(a.build, "tools", "oracle", "oracle_trace")
    for t, what in ((extract, "crt0_extract"), (tracer, "oracle_trace")):
        if not os.path.exists(t):
            die(f"{what} is not built ({t})")
    for e in (a.exe_a, a.exe_b):
        if not os.path.exists(e):
            die(f"no executable at {e}")

    # A's symbolic decode.
    p = subprocess.run([extract, a.exe_a], capture_output=True, text=True)
    if p.returncode != 0:
        die(f"crt0_extract refused on {a.exe_a} (exit {p.returncode})")
    sym = cv.parse_symbolic(p.stdout)

    # B's executed boundary.
    scratch = os.path.join(REPO, "scratch", "oracle", "traces")
    os.makedirs(scratch, exist_ok=True)
    trace = os.path.join(scratch, os.path.basename(a.exe_b) + ".discrim.txt")
    p = subprocess.run([tracer, a.exe_b, "--steps", str(a.steps), "--summary-only", "--out", trace],
                       capture_output=True, text=True)
    if p.returncode != 0:
        die(f"oracle_trace refused on {a.exe_b} (exit {p.returncode})")
    b = cv.parse_boundary(open(trace).read())
    if b is None:
        die(f"the {a.steps}-instruction window on {a.exe_b} never reached the BIOS call, so there is no "
            f"boundary to mismatch against. Raise --steps.")
    regs, _step, _pc, jal = b

    # (label, A value, B value, must_differ)
    rows = [
        ("gp",                 sym.get("gp"),      regs.get("gp"), True),
        ("libcInit (the jal)", sym.get("libcInit"), jal,           True),
        ("crt0_plan sp",       sym.get("planSp"),  regs.get("sp"), True),
        ("crt0_plan a0",       sym.get("planA0"),  regs.get("a0"), True),
        ("crt0_plan a1 (HEAP SIZE)", sym.get("planA1"), regs.get("a1"), True),
        ("BIOS fn $t1",        sym.get("biosFn"),  regs.get("t1"), False),
    ]

    print(f"prove_crossvalidate_discriminates")
    print(f"  symbolic decode of : {a.exe_a}")
    print(f"  executed boundary  : {a.exe_b}")
    print("  These are DIFFERENT programs, so every game-specific field must disagree.\n")

    missing = [lbl for lbl, av, bv, _ in rows if av is None or bv is None]
    if missing:
        die("could not read these field(s) from one or both sources, so the proof would silently cover "
            f"less than it claims: {', '.join(missing)}")

    # THREE outcomes per field, not two. A must-differ field whose two values COINCIDE is not evidence of
    # a broken tool — it can simply be a field these two particular programs happen to share, and calling
    # that FAIL would be a false alarm that trains everyone to ignore this proof. Measured: `crt0_plan sp`
    # is 0x801FFFF8 for BOTH Spyro and Tomba!2, while across the corpus it takes at least three values
    # (0x801FFFF8, 0x80200000, and Spider-Man's 0x807FFFF8) — so it discriminates in general and not for
    # that pair. The honest outcome is INCONCLUSIVE, and the fix is a different pair.
    inconclusive, wrong = [], 0
    for lbl, av, bv, must_differ in rows:
        differs = av != bv
        if must_differ:
            if differs:
                print(f"  ok   {lbl:<26} A=0x{av:08X}  B=0x{bv:08X}  differs (discriminates)")
            else:
                inconclusive.append(lbl)
                print(f"  ??   {lbl:<26} A=0x{av:08X}  B=0x{bv:08X}  INCONCLUSIVE — these two programs "
                      f"share this value")
        else:
            if not differs:
                print(f"  ok   {lbl:<26} A=0x{av:08X}  B=0x{bv:08X}  same, as expected: every PS-X crt0 "
                      f"calls A(39h)")
            else:
                wrong += 1
                print(f"  FAIL {lbl:<26} A=0x{av:08X}  B=0x{bv:08X}  DIFFERS — the constant BIOS call "
                      f"should have matched")

    checked = len(rows)
    print(f"\n  {checked} field(s) checked: {checked - len(inconclusive) - wrong} conclusive, "
          f"{len(inconclusive)} inconclusive, {wrong} wrong.")
    if wrong:
        print("\nFAILED: a field that MUST match across programs did not. The cross-check is misreading one")
        print("  of its two sources.")
        return 1
    if inconclusive:
        print("\nINCONCLUSIVE (exit 2), and that is NOT a pass: these two programs happen to share the")
        print(f"  value of {', '.join(inconclusive)}, so this pair cannot show whether that comparison has")
        print("  any discriminating power. Re-run with a pair that differs there — across this corpus")
        print("  `crt0_plan sp` takes at least three values, so such a pair exists.")
        return 2
    print("\nPROVEN: every game-specific field disagrees across different programs, and only the genuinely")
    print("  constant BIOS function number matches. crossvalidate_crt0.py is comparing two real sources,")
    print("  so its agreements mean something.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
