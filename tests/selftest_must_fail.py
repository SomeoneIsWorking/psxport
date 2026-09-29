#!/usr/bin/env python3
"""Show the decomp selftest can FAIL, by seeding differences into a COPY.

The workspace protocol retires break-the-live-tree as a ritual and asks instead for the case that
WOULD fail, written as a permanent positive test. Those permanent cases live in the selftest. This
script is the separate, weaker evidence: it takes a COPY of tools/ and tests/ into scratch, changes
one defence at a time, and shows the selftest going red each time. Nothing here touches the working
tree, and nothing needs restoring.

TWO THINGS THIS SCRIPT REFUSES TO GET WRONG, both learned the hard way here:

  * A seed whose text no longer matches the shipping code reports **SEED-NOT-APPLIED** rather than
    passing. That happened once: the resident header cross-check was renamed `cross_check_resident`
    when overlay support split the two kinds, the seed kept the old text, and the honest result was
    "this seed now proves nothing" instead of a silent green.
  * A seed **REPLACES** a guard with a pass-through rather than deleting its `if`. Deleting the line
    left a dangling body, so every related test raised NameError and the run went red on the wrong
    subject -- a red that proves the code parses, not that the guard matters. Each seed also names
    the specific check that must fail, so a run that goes red for an unrelated reason is a FAIL.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
PY = [sys.executable]

# (label, file under tools/decomp, text to find, text to put there instead (None = delete),
#  the check that must go red)
SEEDS = [
    ("the non-return warning check is gone from the body test",
     "postscript.py",
     '    if NORETURN_WARNING in c_text:\n',
     None,
     "a body carrying the non-return warning is NOT present"),
    ("the 'a C body with no return' second signal is gone",
     "postscript.py",
     '    if not RETURN_PATTERN.search(c_text):\n',
     None,
     "a C body with no return is NOT present even without the warning"),
    # NOT SEEDED HERE, and the reason is worth stating rather than leaving a gap: the "a target that
    # is a label inside a body is refused" guard lives in the Ghidra-side post-script, so the hermetic
    # selftest cannot reach it -- seeding it here produces a GREEN run, because the check that would
    # catch it reads a REPLAYED inventory rather than executing the script. It is covered where it can
    # be: tests/noreturn_fixture_run.py reaches the same refusal class through a real Ghidra run, and
    # a first version of the real pipeline DID carve that case, producing 4,927 instructions
    # overlapping a 4,994-instruction body.
    ("the audit no longer checks whether a target's body is present",
     "report.py",
     "        elif not target.body_present:\n",
     "        elif False:\n",
     "a target whose body is not present is refused AND NAMED"),
    ("the audit no longer checks that the no-return flag was actually cleared",
     "report.py",
     "    if report.noreturn_still_marked:\n",
     "    if False:\n",
     "a non-return flag that survived the clear is refused"),
    ("the audit no longer refuses an unrecorded pre-script seed",
     "report.py",
     "    if report.preseed is None:\n",
     "    if False:\n",
     "a pre-script that failed is refused, not read as a clean run"),
    # The resident cross-check, by its CURRENT name. It was `cross_check` until overlay support
    # split the two kinds and the resident half became `cross_check_resident`; the seed did not move
    # with the rename and reported SEED-NOT-APPLIED rather than silently passing.
    ("the manifest is no longer cross-checked against the image's own header",
     "images.py",
     "    if spec.text_load_address != header.load:\n",
     "    if False:\n",
     "a manifest load address that disagrees with the header is REFUSED"),
    # A module's load base is trusted as measured DATA, so its SHA-1 gate is the only thing standing
    # between a wrong FILE and a confident answer about the wrong code.
    ("a module is read without its SHA-1 gate",
     "images.py",
     "    if spec.sha1 is not None:\n",
     "    if False:\n",
     "a module whose SHA-1 does not match is REFUSED"),
    ("a module entry missing its measured load base is accepted",
     "images.py",
     "        missing = [name for name in required if getattr(self, name) is None]\n",
     "        missing = []\n",
     "a module entry with no load_base is refused"),
    ("a module's code window is no longer checked against the file",
     "images.py",
     "    if spec.code_last >= size:\n",
     "    if False:\n",
     "a code window that runs past the end of the file is refused"),
    ("the exit-0-with-no-inventory check is gone",
     "headless.py",
     "        if not inventory.is_file():\n",
     "        if False:\n",
     "an exit-0 run with no inventory is a REFUSAL"),
    ("the lock no longer treats EEXIST as contention and refuses on the first try",
     "lock.py",
     "                if error.errno != errno.EEXIST:\n",
     "                if True:\n",
     "it actually retried rather than failing at once"),
    ("an empty target list is accepted",
     "postscript.py",
     '    if not tokens:\n',
     "    if False:\n",
     "a target file with no addresses is refused"),
    ("the body classification stops counting instructions",
     "postscript.py",
     "    if instruction_count <= 0:\n",
     "    if False:\n",
     "a function whose body holds 0 instructions is NOT present"),
]


def run_selftest(root: pathlib.Path) -> tuple[int, str]:
    done = subprocess.run(PY + ["tests/test_decomp_pipeline.py"], cwd=root, capture_output=True,
                          text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def main() -> int:
    results = []
    with tempfile.TemporaryDirectory() as temporary:
        base = pathlib.Path(temporary)
        for index, (label, relative, text, replacement, expected) in enumerate(SEEDS):
            root = base / ("seed%d" % index)
            root.mkdir()
            shutil.copytree(REPO / "tools", root / "tools")
            shutil.copytree(REPO / "tests", root / "tests")
            target = root / "tools" / "decomp" / relative
            source = target.read_text()
            if text not in source:
                results.append((label, "SEED-NOT-APPLIED", expected, ""))
                shutil.rmtree(root, ignore_errors=True)
                continue
            if replacement is None:
                source = source.replace(text, "", 1)
            else:
                source = source.replace(text, replacement, 1)
            target.write_text(source)
            code, output = run_selftest(root)
            results.append((label, "RED" if code != 0 else "STILL-GREEN", expected, output))
            shutil.rmtree(root, ignore_errors=True)

    # And the control: the untouched copy must be GREEN, or "red" above means nothing.
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        shutil.copytree(REPO / "tools", root / "tools")
        shutil.copytree(REPO / "tests", root / "tests")
        code, _output = run_selftest(root)
        control = "GREEN" if code == 0 else "RED"

    print("control (no seed): %s" % control)
    failures = 0
    for label, verdict, expected, output in results:
        if verdict != "RED" or expected not in output:
            failures += 1
        marker = "ok  " if (verdict == "RED" and expected in output) else "FAIL"
        print("  %s %-60s %s" % (marker, label[:60], verdict))
        if verdict != "RED" or expected not in output:
            print("       expected the failing check to name: %r" % expected)
            tail = "\n".join(l for l in output.splitlines() if "FAIL" in l or "FAILED" in l)
            print("       %s" % tail[:500])
    print("")
    if control != "GREEN":
        print("CONTROL FAILED: the untouched copy is not green, so the seeds prove nothing")
        return 1
    if failures:
        print("FAILED: %d of %d seeds did not produce the expected red" % (failures, len(results)))
        return 1
    print("PASSED: control green and %d of %d seeds each turned the selftest RED on its own subject"
          % (len(results), len(results)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
