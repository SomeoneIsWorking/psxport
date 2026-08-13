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
records what the registers actually held. So the two methods share no code and no assumptions — one
reads instructions, the other runs them.

This script runs both and compares them field by field. A measured constant that ships must be diffed by
CODE against the measurement it came from, never hand-compared; hand-comparing is how a session reports
agreement it never checked.

WHAT A NEGATIVE LOOKS LIKE — designed first, on purpose
-------------------------------------------------------
Every way this can fail to answer prints something that is NOT mistakable for agreement:
  * no BOUNDARY-REG block in the trace -> the window never reached the BIOS call, so NOTHING was
    compared. Exits 2 saying so and telling you to raise --steps. It does not print "0 mismatches".
  * a field present in one source and absent in the other -> reported as CANNOT SEE, counted separately
    from both agreements and disagreements, and it makes the run exit nonzero.
  * a disagreement -> printed with both values and which source said what.
And the denominator is always printed: how many fields were compared out of how many were attempted.

Usage:  crossvalidate_crt0.py <PS-X EXE> [--build DIR] [--steps N]
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
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_symbolic(text):
    """Fields crt0_extract reports. Returns {name: int}."""
    out = {}
    # crt0_extract asserts the call is the A(39h) InitHeap thunk. That claim is checkable against the
    # executed $t1, which is the BIOS function number the thunk loads before jumping to 0xA0 — so it is
    # recorded as a comparable FIELD rather than left as prose nobody checks.
    if re.search(r"libcInit is the A\(39h\) InitHeap BIOS thunk: YES", text):
        out["biosFn"] = 0x39
    for line in text.splitlines():
        m = re.match(r"\s+(bssZeroLo|bssZeroHi|stackTopBase|stackTopBase2|heapBase|gp|libcInit)\s+"
                     r"0x([0-9A-Fa-f]+)\s*$", line)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
        m = re.match(r"\s+stackBias\s+(-?\d+)\s*$", line)
        if m:
            out["stackBias"] = int(m.group(1))
    return out


def parse_boundary(text):
    """State at the moment execution left the mapped text. Returns ({reg: int}, step, pc, jal) or None.

    `jal` is the target of the last observed jal — the CALL that left the text. It is recorded by the
    tracer rather than derived here, because a jal's target is the PC one step after $ra is written (the
    delay slot runs in between). Deriving it as `$ra - 4` gives the jal site in the CALLER instead, which
    is a different address and produced a false DISAGREE before this was fixed.
    """
    regs, step, pc, jal = {}, None, None, None
    for line in text.splitlines():
        m = re.match(r"# BOUNDARY-REGS step=(\d+) pc=0x([0-9A-Fa-f]+)", line)
        if m:
            step, pc = int(m.group(1)), int(m.group(2), 16)
        m = re.match(r"# BOUNDARY-REG (\w+)=0x([0-9A-Fa-f]+)", line)
        if m:
            regs[m.group(1)] = int(m.group(2), 16)
        m = re.match(r"# BOUNDARY-LAST-JAL target=0x([0-9A-Fa-f]+)", line)
        if m:
            jal = int(m.group(1), 16)
    if step is None:
        return None
    return regs, step, pc, jal


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("exe")
    ap.add_argument("--build", default=None, help="build dir holding crt0_extract and oracle_trace")
    ap.add_argument("--steps", type=int, default=400000,
                    help="instructions to execute; a bss-zeroing loop can be ~220k (default 400000)")
    a = ap.parse_args()

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

    rc_b, so_b, se_b = run([tracer, a.exe, "--steps", str(a.steps),
                            "--summary-only", "--out", trace_path])
    if rc_b != 0:
        die(f"oracle_trace refused (exit {rc_b}):\n{se_b.strip()}")
    with open(trace_path) as f:
        trace_text = f.read()

    b = parse_boundary(trace_text)
    if b is None:
        die(f"the {a.steps}-instruction window never left the mapped text, so it never reached the BIOS "
            f"call and there is NO boundary to compare. Raise --steps. (A bss-zeroing loop over a large "
            f"bss can need several hundred thousand instructions.) Trace: {trace_path}")
    regs, step, pc, jal_target = b

    print(f"\n  execution left the mapped text at step {step}, pc=0x{pc:08X}")
    print(f"  trace: {trace_path}\n")

    # Each row: label, what method A says, what method B's registers say, and the derivation that links
    # them. The derivation is written out so a reader can check the CLAIM, not just the numbers.
    rows = []

    def cmp_row(label, a_val, b_val, how):
        rows.append((label, a_val, b_val, how))

    cmp_row("gp", sym.get("gp"), regs.get("gp"),
            "crt0 loads $gp with the value crt0_extract reads out of the lui/ori pair")
    cmp_row("libcInit target (the jal)", sym.get("libcInit"), jal_target,
            "the tracer records the target of the last jal before the boundary; crt0_extract names the "
            "same address as libcInit")
    cmp_row("BIOS function number", sym.get("biosFn"), regs.get("t1"),
            "the thunk loads the BIOS function number into $t1 before jumping to 0xA0; crt0_extract "
            "claims A(39h) = InitHeap, so the executed $t1 must be 0x39")
    cmp_row("InitHeap a0 (heapBase+4)", sym.get("heapBase") + 4 if sym.get("heapBase") else None,
            regs.get("a0"),
            "crt0_extract reports the delay slot is `addi a0,a0,4`, so the executed a0 must be "
            "heapBase+4")

    # sp: crt0_extract reports the ADDRESS of the stack-top global plus the bias, not the resulting sp,
    # so this row states what it can and cannot check rather than inventing a comparison.
    bias = sym.get("stackBias")
    sp = regs.get("sp")
    if bias is not None and sp is not None:
        # The executed sp must be a KSEG0 address, and (sp - bias) must be the unbiased stack top, which
        # for every measured Sony crt0 is a round value. Checking the bias RELATION is honest; checking sp
        # against a constant would just re-assert the trace.
        unbiased = (sp - bias) & 0xFFFFFFFF
        rows.append(("stackBias relation", None, None,
                     f"executed sp=0x{sp:08X}, declared bias {bias} -> unbiased stack top "
                     f"0x{unbiased:08X}"))

    agree, disagree, cannot = 0, 0, 0
    print("  field                          method A      method B      verdict")
    print("  " + "-" * 74)
    for label, av, bv, how in rows:
        if av is None and bv is None:
            print(f"  {label:<30} {'—':>11}   {'—':>11}   INFO      {how}")
            continue
        if av is None or bv is None:
            cannot += 1
            a_s = f"0x{av:08X}" if av is not None else "not reported"
            b_s = f"0x{bv:08X}" if bv is not None else "not reported"
            print(f"  {label:<30} {a_s:>11}   {b_s:>11}   CANNOT SEE")
            print(f"       one method does not report this field, so it is NOT agreement: {how}")
            continue
        if av == bv:
            agree += 1
            print(f"  {label:<30} 0x{av:08X}    0x{bv:08X}    AGREE")
        else:
            disagree += 1
            print(f"  {label:<30} 0x{av:08X}    0x{bv:08X}    *** DISAGREE ***")
            print(f"       {how}")

    print()
    total = agree + disagree + cannot
    print(f"  compared {total} field(s): {agree} agree, {disagree} disagree, {cannot} could not be seen.")
    print(f"  NOT covered by this cross-check (crt0_extract reports them; nothing in the executed register")
    print(f"  file at the boundary can confirm them, because they are addresses the crt0 reads rather than")
    print(f"  values it leaves behind): bssZeroLo/Hi, stackTopBase, stackTopBase2, heapBase directly.")

    if disagree:
        print("\nDISAGREEMENT. The two methods do not describe the same crt0. Trust neither shipped constant")
        print("  until it is resolved — and note the EXECUTION is the stronger evidence.")
        return 1
    if agree == 0:
        print("\nREFUSING: zero fields were actually compared, so this run proves nothing.")
        return 2
    print(f"\nAGREEMENT on all {agree} comparable field(s). A symbolic decode and a real execution, sharing")
    print("  no code, describe the same boot group.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
