#!/usr/bin/env python3
"""Does the non-return failure REPRODUCE here, and does the clear stop it? Live Ghidra, opt-in.

WHY THIS IS A SEPARATE TOOL. `test_decomp_pipeline.py` is hermetic and runs in 0.3 s, so it cannot
launch Ghidra. But the defence this pipeline exists for -- `PSXPORT_CLEAR_NORETURN=all` plus the
read-back -- was **asserted and unexercised**: on Spyro 1 the analyzer marked 0 of 673 functions, so
nothing had ever been seen to fail. A defence that has never fired has never been tested, and the
honest thing is to make it fire on a fixture built for the purpose.

WHAT IT MEASURES, and the finding that shaped it. The analyzer's own "Non-Returning Functions -
Discovered" **ran on the fixture and marked nothing** (its row is in the analyzer table, 0 marked of
6 functions). So the mislabel could not be provoked through auto-analysis here, and three attempts
failed first, each producing a clean-looking zero from an instrument that had scanned nothing:

  1. a linear sweep of the fixture decoded **2 instructions** of 336 and found 0 functions, because
     the fixture opens with an infinite branch loop the sweep entered and never left;
  2. seeding only the spinner's entry left **0 functions**, so the post-script decompiled nothing and
     every row read 0 bytes -- which reads as "the failure did not reproduce" and is actually "there
     was nothing to decompile";
  3. `getAnalysisOptions` does not exist as a GhidraScript method; asking for it raises NameError and
     leaves the pre-script unrun while the run reports success. (Same trap the spyro pre-script
     recorded.)

SO THIS SETS THE FLAG ITSELF on the one function that genuinely never returns, and measures BOTH
halves over the SAME caller in ONE process:

  un-cleared  does the caller's C carry `WARNING: Subroutine does not return`, with its body after
              the call discarded?
  cleared     does the same caller's body come back?

Only the second half would pass while the first was broken, which is the "passes but cannot fail"
shape this workspace has paid for nine times.

MEASURED, both halves, Ghidra 12.0.4 on the committed fixture::

    un-cleared  0x800100C0   111 B   2 statements   warning PRESENT
    cleared     0x800100C0    59 B   3 statements   warning absent
    still marked after the clear: 0 of 1

and the un-cleared C is, in full::

    void FUN_800100c0(void) {
      /* WARNING: Subroutine does not return */
      FUN_80010000();
    }

which is the workspace's signature failure, reproduced on purpose and then cured.

Refuses rather than passing vacuously: it needs Ghidra, and it needs the fixture to have produced
functions, so a run that analysed nothing is a FAILURE and not a green "the failure did not occur".
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
TOOLS = HERE.parent / "tools"
for candidate in (str(TOOLS), str(FIXTURES)):
    if candidate not in sys.path:
        sys.path.insert(0, candidate)

from decomp import headless  # noqa: E402

DEFAULT_GHIDRA_HOME = headless.DEFAULT_GHIDRA_HOME
HEAP_MB = 900


class FixtureRefusal(Exception):
    """The fixture could not be run at all. Never reported as 'the failure did not reproduce'."""


def build_fixture(directory: pathlib.Path) -> tuple[pathlib.Path, dict, list[int]]:
    """Build the image in ``directory`` and return it with its descriptor and target list.

    The builder writes beside ITSELF, so it is copied into the work directory first and run there.
    Running it in place would write ``noreturn.bin`` next to the committed generator, which is both a
    tracked-file hazard and not where the run wants the image.
    """
    script = directory / "make_noreturn_fixture.py"
    script.write_text((FIXTURES / "make_noreturn_fixture.py").read_text())
    done = subprocess.run([sys.executable, str(script)], cwd=str(directory),
                          capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise FixtureRefusal("the fixture builder failed: %s" % (done.stderr or done.stdout)[-400:])
    descriptor_path = directory / "noreturn.json"
    if not descriptor_path.is_file():
        raise FixtureRefusal(f"the fixture builder wrote no descriptor at {descriptor_path}")
    descriptor = json.loads(descriptor_path.read_text())
    targets = sorted(int(v, 16) for v in descriptor["functions"].values())
    return directory / "noreturn.bin", descriptor, targets


def run(ghidra_home: pathlib.Path) -> tuple[dict, int]:
    """Run Ghidra once over the fixture. Returns (the measurement, peak RSS bytes)."""
    interpreter = headless.resolve_interpreter(ghidra_home)
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        image, descriptor, targets = build_fixture(work)
        marker = work / "probe.json"
        (work / "probe_targets.json").write_text(
            "\n".join("0x%08X" % t for t in targets) + "\n", encoding="utf-8")
        project = work / "project"
        project.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment["PSXPORT_NORETURN_PROBE"] = str(marker)
        environment["PSXPORT_NORETURN_PROBE_C"] = str(work / "c")
        command = [
            str(interpreter), "-m", headless.LAUNCH_MODULE,
            "--install-dir", str(ghidra_home), "-X", "mx%dm" % HEAP_MB,
            headless.HEADLESS_CLASS, str(project), "noreturn",
            "-import", str(image), "-loader", "BinaryLoader",
            "-loader-baseAddr", descriptor["base"],
            "-processor", "MIPS:LE:32:default",
            "-scriptPath", str(FIXTURES),
            "-preScript", "noreturn_prescript.py",
            "-postScript", "noreturn_chain_postscript.py",
            "-deleteProject",
        ]
        # /usr/bin/time is the only place the peak RSS is observable without a sampling probe, and the
        # memory number is part of the evidence, not an aside.
        timed = ["/usr/bin/time", "-v"] + command
        done = subprocess.run(timed, capture_output=True, text=True, check=False, env=environment)
        log = (done.stdout or "") + (done.stderr or "")
        chained = marker.with_name("probe_chained.json")
        if not chained.is_file():
            raise FixtureRefusal(
                "the fixture run produced no measurement, so nothing was established:\n"
                + "\n".join(log.strip().splitlines()[-14:]))
        measurement = json.loads(chained.read_text())
        peak = 0
        for line in log.splitlines():
            if "Maximum resident set size" in line:
                peak = int(line.rsplit(":", 1)[1].strip()) * 1024
        return measurement, peak


def main() -> int:
    print("no-return fixture: does the failure reproduce, and does the clear stop it?")
    try:
        measurement, peak = run(DEFAULT_GHIDRA_HOME)
    except (FixtureRefusal, headless.GhidraRefusal) as error:
        print("REFUSED: %s" % error)
        return 1

    callers = measurement["callers"]
    uncleared = [c for c in callers if c["phase"] == "uncleared"]
    cleared = [c for c in callers if c["phase"] == "cleared"]
    warned_uncleared = [c for c in uncleared if c["carries_noreturn_warning"]]
    warned_cleared = [c for c in cleared if c["carries_noreturn_warning"]]

    print("")
    print("  the spin function that never returns: %s" % measurement["spin_entry"])
    print("  flagged as non-returning, then cleared: %d; still marked after: %d"
          % (measurement["cleared"], len(measurement["still_marked_after_clear"])))
    print("")
    print("  %-12s %-10s %7s %6s  %s" % ("caller", "phase", "bytes", "stmts", "warning"))
    for row in sorted(callers, key=lambda r: (r["caller"], r["phase"])):
        print("  %-12s %-10s %7d %6d  %s" % (row["caller"], row["phase"], row["c_bytes"],
                                              row["statements"], row["carries_noreturn_warning"]))
    print("  peak RSS: %d KB" % (peak // 1024))
    print("")

    failures = []
    if not uncleared or not cleared:
        failures.append("no caller was decompiled in both phases (%d un-cleared, %d cleared), so the "
                        "comparison is over nothing" % (len(uncleared), len(cleared)))
    if not warned_uncleared:
        failures.append(
            "the un-cleared callers do NOT carry Ghidra's non-return warning, so the failure this "
            "pipeline exists to prevent did not reproduce. The fixture needs revisiting; do NOT read "
            "this as 'the failure does not happen'.")
    if warned_cleared:
        failures.append(
            "%d of %d callers STILL carry the warning after the clear, so the read-back defence is "
            "not effective" % (len(warned_cleared), len(cleared)))
    if measurement["still_marked_after_clear"]:
        failures.append("%d function(s) are still marked non-returning after the clear"
                        % len(measurement["still_marked_after_clear"]))
    # The substantive claim, and it is scoped to the callers that ACTUALLY failed. A first version
    # demanded that clearing restore a body for EVERY caller, and three of four were never truncated
    # -- so it failed on callers the defence had nothing to do with. A check that must hold where
    # there was no failure is a check that cannot fail for the right reason.
    for row in warned_uncleared:
        before = row["statements"]
        after = next((c["statements"] for c in cleared if c["caller"] == row["caller"]), None)
        if after is None:
            failures.append("caller %s failed un-cleared and was not re-decompiled after the clear"
                            % row["caller"])
        elif after <= before:
            failures.append(
                "clearing the flag did not restore the body of %s: %d statement(s) while flagged, %d "
                "after. Restoring the discarded statements is the whole point of the policy."
                % (row["caller"], before, after))
    untouched = [c for c in uncleared if c not in warned_uncleared]
    if untouched:
        # Reported, not failed: these are the CONTROL callers, and a caller that does not truncate is
        # evidence the fixture is not trivially truncating everything.
        print("  control: %d of %d callers were never truncated, so the warning is a property of the "
              "flagged call site and not of every call in the fixture."
              % (len(untouched), len(uncleared)))

    if failures:
        print("FAILED: %d problem(s)" % len(failures))
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("PASSED: the non-return failure reproduced on %d of %d callers and the clear removed it "
          "from all %d, restoring the body in every case."
          % (len(warned_uncleared), len(uncleared), len(cleared)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
