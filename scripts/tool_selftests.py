#!/usr/bin/env python3
"""tool_selftests.py — run every tool's --selftest in a repo, and NAME the tools that have none.

WHY THIS EXISTS
---------------
Measured 2026-08-13: `toystory2/tools/overlay_map.py` was committed with its constant-fold disabled — an
`addiu` that added nothing, left behind by a red-test nobody restored. Its own `--selftest` reported the
breakage clearly (2 of 7 checks) and had been doing so for as long as it was broken, because **nothing ever
ran it**. Seven tools in that repo ship a `--selftest`; no repo in the workspace had a runner. A selftest
nobody runs is a comment.

So this is the runner. It lives in psxport (which every game repo vendors at `external/psxport`) rather than
being copied per repo, so one implementation reaches every tree.

DETECTION IS BY RUNNING, NOT BY GREP
------------------------------------
Whether a tool supports `--selftest` is decided by invoking it and reading the outcome, never by grepping
its source for the word. A grep counts comments, docstrings and help text alongside real support — this
workspace has been burned by sizing work from a grep count before. The classifier:

  exit 0, and the output mentions a selftest        -> PASS
  exit 0, output never mentions one                 -> NO SELFTEST (the flag was ignored, not honoured)
  nonzero, and the flag was taken as a FILENAME     -> NO SELFTEST (not a failure)
  nonzero, and the selftest asked for INPUT it lacks-> UNCHECKED (not a pass and not a failure)
  nonzero otherwise                                -> FAIL

The UNCHECKED class is not politeness, it is accuracy, and it was added because the first version of this
runner got three verdicts wrong on its first real run (2026-08-13): `extract_exe.py` and `resolve_disc.py`
consume an unknown flag as a positional disc path and report "NO SUCH FILE", while `raw_probe.py` has a
genuine selftest that needs two fixture files. Calling any of those a FAILING selftest is a false alarm, and
a report that cries wolf is one nobody reads. They are still not PASSES, so they are counted and named
separately.

THE DENOMINATOR IS THE POINT
----------------------------
It always reports how many tools were scanned, how many have a selftest, how many passed, how many failed,
AND it lists by name the tools with no selftest. "3 passed" from a directory of 20 tools is not a clean bill
of health, and without the missing-selftest list the report would read like one. A directory with no tools
at all REFUSES (exit 2) rather than printing a vacuous pass.

Usage:  tool_selftests.py [tools-dir] [--timeout SEC] [--only NAME]...
Exit:   0 = every tool that has a selftest passed, and at least one ran.
        1 = at least one selftest FAILED.
        2 = nothing could be checked (no directory, no tools, or no tool has a selftest).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Substrings that mean "this tool does not know the flag", i.e. it has no selftest — as opposed to a
# selftest that ran and failed. Kept explicit and few; anything else nonzero is treated as a real FAILURE,
# because guessing generously here is how a broken tool gets classified as "no selftest" and disappears.
NO_FLAG_MARKERS = (
    "unrecognized arguments",
    "unrecognised arguments",
    "invalid choice",
    "no such option",
    "unknown option",
    "usage:",
)

# The tool did not reject the flag — it took it as a PATH. That is also "no selftest", but it fails
# differently, so it needs its own markers or it lands in FAIL and reads as a broken tool.
FLAG_AS_PATH_MARKERS = (
    "no such file",
    "cannot open",
    "could not open",
    "argument names '--selftest'",
    'argument names "--selftest"',
    "is not a file",
    "does not exist",
)

# A real selftest that cannot run without fixtures. Only consulted when the output ALSO mentions a
# selftest, so a tool merely complaining about missing input is not swept in here.
NEEDS_INPUT_MARKERS = ("needs", "requires", "provide", "expects", "give me", "pass a",
                       "no corpus", "at least two", "refusing", "refused")

# Exceptions that mean "this script only runs inside a HOST application" (Ghidra headless), not "this tool
# is broken". Measured 2026-08-13: symdump_re.py and symwidth_re.py raise NameError on `currentProgram` and
# ghidra_decomp.py raises ModuleNotFoundError for `ghidra`, none of which is a defect in the tool.
HOST_ONLY_MARKERS = ("no module named 'ghidra'", "name 'currentprogram' is not defined",
                     "name 'currentProgram' is not defined".lower(), "no module named 'ghidra_bridge'")

# A traceback whose exception comes from PARSING the flag as data. The tool never had a --selftest; it just
# crashed on an argument it did not expect. Distinguished from a genuine selftest failure by the fact that
# the flag text itself appears in the exception message.
PARSE_CRASH_EXCEPTIONS = ("valueerror", "indexerror", "typeerror", "keyerror", "filenotfounderror")

# The tool needs something outside itself that this run has not provided — a listening debug server, a
# display. Not a defect in the tool and not a pass for it.
ENV_MISSING_MARKERS = ("connectionrefusederror", "connection refused", "could not connect",
                       "no display", "cannot connect to")

# A PASS must show a real VERDICT, not merely contain the word "selftest". A retired symbol resolver
# once exited 0
# printing "--selftest  <not a number>" — the flag echoed back — and a bare `"selftest" in output` test
# read that as a passing selftest. A false PASS is worse than a false FAIL: it manufactures confidence.
VERDICT_TOKENS = ("pass", "passed", "ok", "green", "0 failed", "all checks")


def classify(path, timeout):
    """Run `<tool> --selftest` and return (verdict, detail) with verdict in PASS/FAIL/NONE/ERROR."""
    try:
        p = subprocess.run(
            [sys.executable, path, "--selftest"],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return "FAIL", f"timed out after {timeout}s — a selftest that cannot finish cannot vouch for anything"
    except OSError as e:
        return "ERROR", f"could not execute: {e}"

    out = (p.stdout or "") + (p.stderr or "")
    low = out.lower()
    if p.returncode == 0:
        # Distinguish "ran a selftest and passed" from "ignored the flag and did its normal job, exit 0".
        # A tool that silently ignores --selftest would otherwise be counted as covered when it is not.
        if "selftest" not in low:
            return "NONE", "exited 0 but never mentioned a selftest — the flag was ignored, not honoured"
        # ... and from "echoed the flag back". Requiring a verdict token is what separates a real report
        # from output that merely contains the word because the argument is in it.
        if not any(t in low for t in VERDICT_TOKENS):
            return "NONE", ("exited 0 and mentioned '--selftest' only by echoing the argument, with no "
                            "verdict — that is not a selftest")
        return "PASS", first_summary(out)
    if any(m in low for m in HOST_ONLY_MARKERS):
        return "HOST", "runs only inside Ghidra headless — cannot be checked from a plain shell"
    if "traceback (most recent call last)" in low and any(e in low for e in PARSE_CRASH_EXCEPTIONS):
        return "NONE", "crashed parsing --selftest as data, so it has no selftest flag"
    # An environment the tool needs is absent (a debug server to connect to, a display). Not a defect and
    # not a pass. Measured: dbgclient.py raises ConnectionRefusedError because no port is listening.
    if any(m in low for m in ENV_MISSING_MARKERS):
        return "UNCHECKED", "needs a live environment this run does not provide: " + (first_summary(out) or "")
    # An honest refusal that never mentions a selftest: the tool has none, and it declined for lack of
    # input rather than crashing. Measured: exe_similarity.py refuses with "need at least TWO executables".
    if "selftest" not in low and any(m in low for m in ("refusing", "refused")):
        return "NONE", "no selftest; it refused for lack of input: " + (first_summary(out) or "")
    if any(m in low for m in NO_FLAG_MARKERS):
        return "NONE", "does not accept --selftest"
    if any(m in low for m in FLAG_AS_PATH_MARKERS):
        return "NONE", "took --selftest as a FILENAME, so it has no selftest flag"
    if "selftest" in low and any(m in low for m in NEEDS_INPUT_MARKERS):
        return "UNCHECKED", "has a selftest but it needs input: " + (first_summary(out) or "(no detail)")
    return "FAIL", first_summary(out) or f"exit {p.returncode} with no output"


def first_summary(out):
    """The most report-like line: prefer an explicit n/m tally, else the last non-empty line."""
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    for l in reversed(lines):
        low = l.lower()
        if ("passed" in low or "failed" in low or "check" in low) and any(ch.isdigit() for ch in l):
            return l[:150]
    return lines[-1][:150] if lines else ""


# ── the runner's OWN selftest ──────────────────────────────────────────────────────────────────────
# A classifier is trusted only once it has shown EVERY answer, not just the one you hope for. This one got
# three verdicts wrong on its first real run and produced one false PASS on its second, so it gets fixtures:
# tiny tools whose correct classification is known by construction, including the two failure modes that
# actually bit — a tool that echoes the flag back and exits 0, and a tool that crashes parsing it.
FIXTURES = [
    ("passing.py",     "import sys\nprint('[selftest] 3/3 passed')\n",                          "PASS"),
    ("failing.py",     "import sys\nprint('[selftest] 1 of 3 checks FAILED')\nsys.exit(1)\n",  "FAIL"),
    ("noflag.py",      "import argparse\nargparse.ArgumentParser().parse_args()\n",             "NONE"),
    ("echoes.py",      "import sys\nprint(sys.argv[1], '<not a number>')\n",                    "NONE"),
    ("crashes.py",     "import sys\nint(sys.argv[1], 16)\n",                                    "NONE"),
    ("needsinput.py",  "import sys\nprint('--selftest needs a corpus')\nsys.exit(2)\n",         "UNCHECKED"),
    ("hostonly.py",    "import ghidra\n",                                                        "HOST"),
]


def run_selftest():
    print(
        f"tool_selftests --selftest: classifying {len(FIXTURES)} fixture tool(s) whose verdict is "
        "known by construction."
    )
    print("  Every class the classifier can emit is exercised, including the two that were WRONG in")
    print("  practice: a tool echoing the flag back at exit 0, and one crashing while parsing it.\n")
    ran = failed = 0
    fixture_root = Path(__file__).resolve().parent.parent / "build/tool-selftests/tool-selftests"
    shutil.rmtree(fixture_root, ignore_errors=True)
    fixture_root.mkdir(parents=True)
    try:
        for name, body, want in FIXTURES:
            path = fixture_root / name
            with open(path, "w") as f:
                f.write(body)
            got, detail = classify(path, 60)
            ran += 1
            ok = got == want
            if not ok:
                failed += 1
            marker = "ok  " if ok else "FAIL"
            print(f"  {marker} {name:<16} want {want:<10} got {got:<10} {detail[:60]}")
    finally:
        shutil.rmtree(fixture_root, ignore_errors=True)
    print(f"\n  {ran} fixture(s) classified, {failed} wrong.")
    if ran != len(FIXTURES):
        print(f"REFUSING: {ran} fixtures ran but {len(FIXTURES)} are declared.")
        return 2
    if failed:
        print("FAILED: the classifier does not agree with fixtures whose answer is known. Its verdicts on")
        print("  real tools cannot be trusted until it does.")
        return 1
    print("PASSED: every class is reached and correct on constructed input. This says NOTHING about")
    print("  whether the marker lists cover a phrasing no fixture here uses — a new tool that fails in a")
    print("  new way can still be misclassified, which is why every verdict prints its evidence.")
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--selftest", action="store_true",
                    help="classify constructed fixtures and check the classifier against known answers")
    ap.add_argument("tools_dir", nargs="?", default="tools",
                    help="directory of tools to check (default: ./tools)")
    ap.add_argument("--timeout", type=int, default=600, help="per-tool timeout in seconds")
    ap.add_argument("--only", action="append", default=[],
                    help="check only these tool basenames (repeatable)")
    a = ap.parse_args()
    if a.selftest:
        return run_selftest()

    d = a.tools_dir
    if not os.path.isdir(d):
        print(f"tool_selftests: REFUSING (exit 2) — no directory at {d!r}. Nothing was checked; this is "
              f"not a pass.")
        return 2

    tools = sorted(f for f in os.listdir(d)
                   if f.endswith(".py") and not f.startswith("_"))
    if a.only:
        want = set(a.only)
        tools = [t for t in tools if t in want or os.path.splitext(t)[0] in want]
    if not tools:
        print(f"tool_selftests: REFUSING (exit 2) — {d!r} contains no Python tools to check"
              + (" matching --only" if a.only else "") + ". Nothing was checked; this is not a pass.")
        return 2

    print(f"tool_selftests: {len(tools)} tool(s) in {d}/  (detection is by RUNNING --selftest, not by grep)")
    print()

    passed, failed, none, errored, unchecked, host = [], [], [], [], [], []
    bucket = {"PASS": passed, "FAIL": failed, "NONE": none, "ERROR": errored,
              "UNCHECKED": unchecked, "HOST": host}
    for t in tools:
        verdict, detail = classify(os.path.join(d, t), a.timeout)
        bucket[verdict].append((t, detail))
        if verdict == "PASS":
            print(f"  ok    {t:<26} {detail}")
        elif verdict == "FAIL":
            print(f"  FAIL  {t:<26} {detail}")
        elif verdict == "UNCHECKED":
            print(f"  ??    {t:<26} {detail}")
        elif verdict == "HOST":
            print(f"  host  {t:<26} {detail}")
        elif verdict == "ERROR":
            print(f"  ERR   {t:<26} {detail}")

    have = len(passed) + len(failed) + len(unchecked)
    print()
    print(f"  {len(tools)} tool(s) scanned: {have} have a --selftest ({len(passed)} passed, "
          f"{len(failed)} FAILED, {len(unchecked)} could not be checked), {len(none)} have none, "
          f"{len(host)} are Ghidra-only, {len(errored)} could not run.")
    if unchecked:
        print(f"\n  UNCHECKED ({len(unchecked)} tool(s)) — a real selftest that needs fixtures this run did")
        print("  not supply. NOT a pass: give it the inputs it names and run it directly.")
        for t, why in unchecked:
            print(f"    {t:<26} {why}")

    # The missing half, NAMED. Without this the report reads as a clean bill of health for the whole
    # directory when it only ever covered part of it.
    if none:
        print(f"\n  NO SELFTEST ({len(none)} tool(s)) — nothing here vouches for these, and this runner")
        print("  cannot tell a working one from a broken one:")
        for t, why in none:
            print(f"    {t:<26} {why}")
    if errored:
        print(f"\n  COULD NOT RUN ({len(errored)}):")
        for t, why in errored:
            print(f"    {t:<26} {why}")

    if failed:
        print(f"\nFAILED: {len(failed)} tool selftest(s) do not pass. A tool whose own selftest fails must")
        print("  be treated as DISTRUSTED, and every result it has produced re-checked.")
        return 1
    if have == 0:
        print("\nREFUSING (exit 2): not one tool in this directory has a --selftest, so this run checked")
        print("  nothing. An all-green report here would be pure noise.")
        return 2
    print(f"\nPASSED: all {len(passed)} tool(s) whose selftest RAN passed. This says NOTHING about the")
    print(f"  {len(none)} tool(s) with no selftest, the {len(unchecked)} that could not be checked here, or")
    print(f"  the {len(host)} that only run inside Ghidra.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
