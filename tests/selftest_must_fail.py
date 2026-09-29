#!/usr/bin/env python3
"""Show the decomp selftest can FAIL, by seeding differences into a COPY.

The workspace protocol retires break-the-live-tree as a ritual and asks instead for the case that
WOULD fail, written as a permanent positive test. Those permanent cases live in the selftest. This
script is the separate, weaker evidence: it takes a COPY of tools/ and tests/ into scratch, removes
one defence at a time, and shows the selftest going red each time. Nothing here touches the working
tree, and nothing needs restoring.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
PY = [sys.executable]

# (label, file relative to tools/decomp, the exact text to remove, what must go red)
SEEDS = [
    ("the non-return warning check is gone from the body test",
     "postscript.py",
     '    if NORETURN_WARNING in c_text:\n'
     '        return False, ("C carries Ghidra\'s non-return warning, so the body after the call was "\n'
     '                       "discarded and a return fabricated")\n',
     "a body carrying the non-return warning is NOT present"),
    ("the 'a C body with no return' second signal is gone",
     "postscript.py",
     '    if not RETURN_PATTERN.search(c_text):\n'
     '        return False, "C has no return statement, so it is not a complete function body"\n',
     "a C body with no return is NOT present even without the warning"),
    ("the audit no longer checks whether a target's body is present",
     "report.py",
     "        elif not target.body_present:\n"
     "            problems.append(\n"
     '                "target %s (%s) decompiled to %d byte(s) but its body is NOT present: %s"\n'
     '                % (target.requested, target.name or "-", target.c_bytes, target.body_reason))\n',
     "a target whose body is not present is refused AND NAMED"),
    ("the audit no longer checks that the no-return flag was actually cleared",
     "report.py",
     "    if report.noreturn_still_marked:\n",
     "a non-return flag that survived the clear is refused"),
    ("the manifest is no longer cross-checked against the image's own header",
     "images.py",
     "    if spec.text_load_address != header.load:\n"
     "        raise ImageRefusal(\n"
     '            f"{spec.title} ({spec.serial}) manifest says text loads at '
     '0x{spec.text_load_address:08X} "\n'
     '            f"but {path} says 0x{header.load:08X}. Every guest address the pipeline reports '
     'would be "\n'
     '            "wrong. Fix the manifest entry, not the base."\n'
     "        )\n",
     "a manifest load address that disagrees with the header is REFUSED"),
    ("the exit-0-with-no-inventory check is gone",
     "headless.py",
     "        if not inventory.is_file():\n",
     "an exit-0 run with no inventory is a REFUSAL"),
    # Removing the errno test makes EEXIST an immediate refusal instead of a retry, so the check
    # that fails is the one asserting it actually WAITED. That is the subject: a lock that gives up
    # on the first EEXIST is how two analyses end up running at once.
    ("the lock no longer treats EEXIST as contention and refuses on the first try",
     "lock.py",
     "                if error.errno != errno.EEXIST:\n",
     "it actually retried rather than failing at once"),
    ("an empty target list is accepted",
     "postscript.py",
     '    if not tokens:\n'
     '        raise PolicyRefusal(\n'
     '            f"the target list at {path} is empty. Refusing: \'0 of 0 targets decompiled\' is a '
     'bug, "\n'
     '            "not a result."\n'
     '        )\n',
     "a target file with no addresses is refused"),
]


def run_selftest(root: pathlib.Path) -> tuple[int, str]:
    done = subprocess.run(PY + ["tests/test_decomp_pipeline.py"], cwd=root, capture_output=True,
                          text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def main() -> int:
    results = []
    with tempfile.TemporaryDirectory() as temporary:
        base = pathlib.Path(temporary)
        for label, relative, text, expected in SEEDS:
            root = base / ("seed%d" % len(results))
            (root).mkdir()
            shutil.copytree(REPO / "tools", root / "tools")
            shutil.copytree(REPO / "tests", root / "tests")
            target = root / "tools" / "decomp" / relative
            source = target.read_text()
            if text not in source:
                results.append((label, "SEED-NOT-APPLIED", expected, ""))
                shutil.rmtree(root, ignore_errors=True)
                continue
            target.write_text(source.replace(text, "", 1))
            code, output = run_selftest(root)
            red = code != 0
            named = expected in output
            results.append((label, "RED" if red else "STILL-GREEN", expected, output))
            shutil.rmtree(root, ignore_errors=True)

    # And the control: the untouched copy must be GREEN, or "red" above means nothing.
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        shutil.copytree(REPO / "tools", root / "tools")
        shutil.copytree(REPO / "tests", root / "tests")
        code, output = run_selftest(root)
        control = "GREEN" if code == 0 else "RED"

    print("control (no seed): %s" % control)
    failures = 0
    for label, verdict, expected, output in results:
        if verdict != "RED" or expected not in output:
            failures += 1
        marker = "ok  " if (verdict == "RED" and expected in output) else "FAIL"
        print("  %s %-62s %s" % (marker, label[:62], verdict))
        if verdict == "STILL-GREEN" or expected not in output:
            tail = "\n".join(l for l in output.splitlines() if "FAIL" in l or "FAILED" in l)
            print("       expected the failing check to name: %r" % expected)
            print("       %s" % tail[:600])
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
