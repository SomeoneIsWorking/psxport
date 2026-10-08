#!/usr/bin/env python3
"""crossvalidate_crt0.py — check the crt0 boot group two INDEPENDENT ways and diff them BY CODE.

WHY THIS EXISTS
---------------
`tools/crt0_extract` derives a game's boot group SYMBOLICALLY: it decodes the crt0 prologue without
running it and reports bssZeroLo/Hi, stackTopBase, stackBias, heapBase, gp and libcInit. Those numbers
then ship, in each game's `game_config.cpp`, and `crt0_audit` re-derives them every boot. But every one
of those checks uses THE SAME DECODER. A wrong reading of the prologue would be confirmed by all of
them, forever, which is the exact failure the oracle exists to break: an instrument that cannot show the
other answer.

`tools/oracle/oracle_trace` derives nothing. It EXECUTES the real crt0 in the vendored Mednafen CPU and
records the first call boundary independently. For stock A(39h) thunks it also retains the later BIOS
boundary check; an in-image libcInit does not have to leave mapped text.

This script runs both and compares them field by field. A measured constant that ships must be diffed by
CODE against the measurement it came from, never hand-compared; hand-comparing is how a session reports
agreement it never checked.

WHAT A NEGATIVE LOOKS LIKE — designed first, on purpose
-------------------------------------------------------
Every way this can fail to answer prints something that is NOT mistakable for agreement:
  * no CALL-BOUNDARY-REG block in the trace -> the window never reached a call, so NOTHING was compared.
    Exits 2 saying so and telling you to raise --steps. It does not print "0 mismatches".
  * a field present in one source and absent in the other -> reported as CANNOT SEE, counted separately
    from both agreements and disagreements, and it makes the run exit nonzero.
  * a disagreement -> printed with both values and which source said what.
And the denominator is always printed: how many fields were compared out of how many were attempted.

Usage:  crossvalidate_crt0.py <PS-X EXE> [--build DIR] [--steps N]
        crossvalidate_crt0.py --selftest
Exit:   0 = every comparable field agrees, and at least one was compared.
        1 = a real disagreement.
        2 = could not compare (a tool missing, the window too short, a field invisible).
"""
import argparse
import os
import re
import subprocess
import sys


def die(msg, code=2):
    print(f"crossvalidate_crt0: REFUSING (exit {code}): {msg}")
    print("  Nothing was compared. This is not agreement.")
    sys.exit(code)


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    return p.returncode, p.stdout, p.stderr


def parse_symbolic(text):
    """Fields crt0_extract reports. Returns {name: int}."""
    out = {}
    # crt0_extract asserts the call is the A(39h) InitHeap thunk. That claim is checkable against the
    # executed $t1, which is the BIOS function number the thunk loads before jumping to 0xA0 — so it is
    # recorded as a comparable FIELD rather than left as prose nobody checks.
    if re.search(r"libcInit is the A\(39h\) InitHeap BIOS thunk: YES", text):
        out["biosFn"] = 0x39
    # What the SHIPPING arithmetic (`crt0_plan`) makes of the scan. This is the line that lets a1 — the
    # InitHeap heap SIZE — be compared at all: it is not a scanned field, it is computed from two words the
    # crt0 loads, and it is the field that was actually WRONG before (every port passed size 0).
    m = re.search(r"crt0_plan \(THE shipping arithmetic[^)]*\): sp=0x([0-9A-Fa-f]+) gp=0x([0-9A-Fa-f]+)"
                  r" InitHeap\(a0=0x([0-9A-Fa-f]+), a1=0x([0-9A-Fa-f]+)\)", text)
    if m:
        out["planSp"] = int(m.group(1), 16)
        out["planGp"] = int(m.group(2), 16)
        out["planA0"] = int(m.group(3), 16)
        out["planA1"] = int(m.group(4), 16)
    for line in text.splitlines():
        m = re.match(r"\s+(bssZeroLo|bssZeroHi|stackTopBase|stackTopBase2|heapBase|gp|libcInit)\s+"
                     r"0x([0-9A-Fa-f]+)\s*$", line)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
        m = re.match(r"\s+stackBias\s+(-?\d+)\s*$", line)
        if m:
            out["stackBias"] = int(m.group(1))
    return out


def parse_boundary(text, tag="BOUNDARY"):
    """Parse one named register boundary. Returns ({reg: int}, step, pc, jal) or None.

    `jal` is the target of the last observed jal — the CALL that left the text. It is recorded by the
    tracer rather than derived here, because a jal's target is the PC one step after $ra is written (the
    delay slot runs in between). Deriving it as `$ra - 4` gives the jal site in the CALLER instead, which
    is a different address and produced a false DISAGREE before this was fixed.
    """
    regs, step, pc, jal = {}, None, None, None
    regs_re = re.compile(rf"# {re.escape(tag)}-REGS step=(\d+) pc=0x([0-9A-Fa-f]+)")
    reg_re = re.compile(rf"# {re.escape(tag)}-REG (\w+)=0x([0-9A-Fa-f]+)")
    for line in text.splitlines():
        m = regs_re.match(line)
        if m:
            step, pc = int(m.group(1)), int(m.group(2), 16)
        m = reg_re.match(line)
        if m:
            regs[m.group(1)] = int(m.group(2), 16)
        marker = "BOUNDARY-LAST-JAL" if tag == "BOUNDARY" else "CAPTURED-CALL"
        m = re.match(rf"# {marker} target=0x([0-9A-Fa-f]+)", line)
        if m:
            jal = int(m.group(1), 16)
    if step is None:
        return None
    return regs, step, pc, jal


def comparison_rows(sym, call, text_exit=None):
    regs, _, _, jal = call
    rows = [
        ("gp", sym.get("gp"), regs.get("gp"), "crt0 loads the decoded value into $gp"),
        ("libcInit target (the jal)", sym.get("libcInit"), jal,
         "the oracle's first executed jal must target decoded libcInit"),
        ("libcInit a0 (heapBase+4)", sym.get("heapBase") + 4 if sym.get("heapBase") else None,
         regs.get("a0"), "the decoded delay slot adds four to heapBase"),
        ("crt0_plan sp", sym.get("planSp"), regs.get("sp"), "executed $sp must equal the shipping plan"),
        ("crt0_plan a0", sym.get("planA0"), regs.get("a0"), "executed $a0 must equal the shipping plan"),
        ("crt0_plan a1 (HEAP SIZE)", sym.get("planA1"), regs.get("a1"),
         "executed $a1 at libcInit must equal the shipping plan"),
    ]
    if "biosFn" in sym:
        bios = None
        if text_exit and text_exit[3] == sym.get("libcInit"):
            bios = text_exit[0].get("t1")
        rows.append(("BIOS function number", sym["biosFn"], bios,
                     "the decoded stock thunk must be the call exiting at A(39h)"))
    bias, sp = sym.get("stackBias"), regs.get("sp")
    if bias is not None and sp is not None:
        rows.append(("stackBias relation", None, None,
                     f"executed sp=0x{sp:08X}, bias {bias} -> top 0x{(sp - bias) & 0xFFFFFFFF:08X}"))
    return rows


def row_counts(rows):
    compared = [(a, b) for _, a, b, _ in rows if a is not None or b is not None]
    return (sum(a == b for a, b in compared if a is not None and b is not None),
            sum(a != b for a, b in compared if a is not None and b is not None),
            sum(a is None or b is None for a, b in compared))


def selftest():
    crash = {"gp": 0x800563FC, "libcInit": 0x80011A18, "heapBase": 0x80061A78,
             "planSp": 0x801FFFF8, "planA0": 0x80061A7C, "planA1": 0x19D580, "stackBias": -8}
    call_text = """# CAPTURED-CALL target=0x80011A18 ra=0x8003E0A8 step=57910
# CALL-BOUNDARY-REGS step=57910 pc=0x80011A18
# CALL-BOUNDARY-REG gp=0x800563FC
# CALL-BOUNDARY-REG sp=0x801FFFF8
# CALL-BOUNDARY-REG a0=0x80061A7C
# CALL-BOUNDARY-REG a1=0x0019D580"""
    call = parse_boundary(call_text, "CALL-BOUNDARY")
    ctr = dict(crash, biosFn=0x39)
    exit_text = """# BOUNDARY-REGS step=57913 pc=0x000000A0
# BOUNDARY-LAST-JAL target=0x80011A18 ra=0x8003E0A8 step=57910
# BOUNDARY-REG t1=0x00000039"""
    checks = [
        ("Crash in-image libcInit", row_counts(comparison_rows(crash, call)) == (6, 0, 0)),
        ("unreached call refuses", parse_boundary("# FIRST CALL WAS NOT REACHED", "CALL-BOUNDARY") is None),
        ("wrong call target disagrees", row_counts(comparison_rows(
            crash, parse_boundary(call_text.replace("target=0x80011A18", "target=0x80011A1C"),
                                  "CALL-BOUNDARY"))) == (5, 1, 0)),
        ("CTR A(39h) control", row_counts(comparison_rows(ctr, call, parse_boundary(exit_text))) == (7, 0, 0)),
        ("unrelated exit is unseen", row_counts(comparison_rows(
            ctr, call, parse_boundary(exit_text.replace("target=0x80011A18", "target=0x80011A1C")))) == (6, 0, 1)),
    ]
    for label, ok in checks:
        print(f"  {'PASS' if ok else 'FAIL'} {label}")
    failed = sum(not ok for _, ok in checks)
    print(f"crossvalidate_crt0 --selftest: {len(checks) - failed}/{len(checks)} passed")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("exe", nargs="?")
    ap.add_argument("--build", default=None, help="build dir holding crt0_extract and oracle_trace")
    ap.add_argument("--steps", type=int, default=400000,
                    help="instructions to execute; a bss-zeroing loop can be ~220k (default 400000)")
    ap.add_argument("--selftest", action="store_true", help="run bounded Crash/CTR boundary tests")
    a = ap.parse_args()
    if a.selftest:
        return selftest() if not a.exe else die("--selftest does not take an executable")
    if not a.exe:
        die("no executable given")

    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build = a.build or os.path.join(repo, "build")
    extract = os.path.join(build, "tools", "crt0_extract")
    tracer = os.path.join(build, "tools", "oracle", "oracle_trace")

    if not os.path.exists(a.exe):
        die(f"no executable at {a.exe}")
    for t, what in ((extract, "crt0_extract"), (tracer, "oracle_trace")):
        if not os.path.exists(t):
            die(f"{what} is not built ({t}). Both methods are required — comparing one method against "
                f"itself would prove nothing.")

    scratch = os.path.join(repo, "scratch", "oracle", "traces")
    os.makedirs(scratch, exist_ok=True)
    trace_path = os.path.join(scratch, os.path.basename(a.exe) + ".boundary.txt")

    print(f"crossvalidate_crt0: {a.exe}")
    print("  method A: crt0_extract  — DECODES the prologue without running it")
    print("  method B: oracle_trace  — EXECUTES it in the vendored Mednafen CPU (no libretro.c, no BIOS)")
    print(f"  the two share no code. window: {a.steps} instruction(s)")

    rc_a, so_a, se_a = run([extract, a.exe])
    if rc_a != 0:
        die(f"crt0_extract refused (exit {rc_a}):\n{se_a.strip()}")
    sym = parse_symbolic(so_a)
    if not sym:
        die("crt0_extract produced no parseable boot-group fields — its report format changed and this "
            "script would silently compare nothing")

    if sym.get("libcInit") is None:
        die("crt0_extract did not report libcInit; there is no call boundary to compare")
    rc_b, _so_b, se_b = run([tracer, a.exe, "--steps", str(a.steps), "--capture-first-call",
                             "--summary-only", "--out", trace_path])
    if rc_b != 0:
        die(f"oracle_trace refused (exit {rc_b}):\n{se_b.strip()}")
    with open(trace_path) as f:
        trace_text = f.read()

    call = parse_boundary(trace_text, "CALL-BOUNDARY")
    if call is None:
        die(f"the {a.steps}-instruction window never reached a call, so there is NO boundary to compare. "
            f"Raise --steps. Trace: {trace_path}")
    _regs, step, pc, _jal = call
    text_exit = parse_boundary(trace_text)

    print(f"\n  execution reached the first crt0 call at step {step}, pc=0x{pc:08X}")
    if "biosFn" in sym and text_exit:
        print(f"  stock A(39h) control also reached pc=0x{text_exit[2]:08X} at step {text_exit[1]}")
    print(f"  trace: {trace_path}\n")

    rows = comparison_rows(sym, call, text_exit)

    agree, disagree, cannot = row_counts(rows)
    print("  field                          method A      method B      verdict")
    print("  " + "-" * 74)
    for label, av, bv, how in rows:
        if av is None and bv is None:
            print(f"  {label:<30} {'—':>11}   {'—':>11}   INFO      {how}")
            continue
        if av is None or bv is None:
            a_s = f"0x{av:08X}" if av is not None else "not reported"
            b_s = f"0x{bv:08X}" if bv is not None else "not reported"
            print(f"  {label:<30} {a_s:>11}   {b_s:>11}   CANNOT SEE")
            print(f"       one method does not report this field, so it is NOT agreement: {how}")
            continue
        if av == bv:
            print(f"  {label:<30} 0x{av:08X}    0x{bv:08X}    AGREE")
        else:
            print(f"  {label:<30} 0x{av:08X}    0x{bv:08X}    *** DISAGREE ***")
            print(f"       {how}")

    print()
    total = agree + disagree + cannot
    print(f"  compared {total} field(s): {agree} agree, {disagree} disagree, {cannot} could not be seen.")
    print("  NOT covered by this cross-check: bssZeroLo/Hi, stackTopBase, stackTopBase2 and heapBase as")
    print("  ADDRESSES. They are locations the crt0 reads, not values it leaves in a register, so the")
    print("  boundary register file cannot confirm them. (bssZeroLo/Hi are corroborated indirectly — the")
    print("  first four traced instructions load exactly those two values into $v0/$v1 — but this script")
    print("  does not assert that, so it is not counted above.)")

    if disagree:
        print("\nDISAGREEMENT. The two methods do not describe the same crt0. Trust neither shipped constant")
        print("  until it is resolved — and note the EXECUTION is the stronger evidence.")
        return 1
    if cannot:
        print("\nREFUSING: at least one required field could not be observed at its validated boundary.")
        return 2
    if agree == 0:
        print("\nREFUSING: zero fields were actually compared, so this run proves nothing.")
        return 2
    print(f"\nAGREEMENT on all {agree} comparable field(s). A symbolic decode and a real execution, sharing")
    print("  no code, describe the same libcInit call boundary.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
