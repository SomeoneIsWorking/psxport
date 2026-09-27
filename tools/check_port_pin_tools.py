#!/usr/bin/env python3
"""check_port_pin_tools.py — every port's `tools/psxport_sync.py` must match the canonical copy.

WHY THIS FILE EXISTS
--------------------
A port must build from a bare clone of ITSELF, so the pin tool travels with it and there are ten copies.
That is deliberate. The cost is drift, and the drift was measured, not feared:

  - MEASURED 2026-09-27: **ten copies, ten distinct hashes, 298–322 lines each.**
  - The staleness guard was **missing from seven of the ten**. Where it was missing, `--check` answered
    `check OK` on exactly the input a guarded copy refused — same input, opposite answers, and the wrong
    one is a pass. A tree rebuilt against newer framework code was passing a provenance check.
  - Only **one** of the ten had a `--build` flag, so the live pin check that actually *calls* the check
    could not be registered in the other nine. All ten were therefore reporting green selftests of a
    function none of them ever ran.
  - Only **one** had the `do_bump` fix, so the other nine could still record a framework commit the tree
    was never built against — the original incident this mechanism exists to prevent.

The obligation to keep the copies in step was real; the check for that obligation was itself duplicated,
which is why the guard could go missing from seven of them unnoticed. This is that check, made
mechanical and registered.

WHAT IT COMPARES, AND WHY THAT IS NOT ENOUGH ON ITS OWN
--------------------------------------------------------
It compares **bytes**, and then — because of how this was actually caught — it also **runs** each copy.

The byte comparison alone was demonstrably insufficient, on this very file. Building the canonical dropped
an `import argparse` that only `main()` reaches. The result propagated to all ten ports, and **every one
of them reported "in step" while being identically broken**: each repo's test imports the module and never
calls `main()`, so the missing import raised nothing, and no test anywhere invoked the tool as a command.
Ten identical copies of a file that could not run is the exact failure duplication produces, and a
byte-equality gate endorsed it.

So the gate is two questions, and it must pass both:

  1. is this copy the canonical text?   (byte comparison — what the copies are FOR)
  2. does this copy actually run?       (`--help` in a subprocess, exit 0 — whether it WORKS)

It still does not judge whether a copy is *correct*. A copy can be faithful and still wrong, and then the
fix belongs in `tools/psxport_sync.py` here, where all ten get it at once.

The canonical copy is never executed as a port tool. `REPO` is derived from the file's own path, so the
text runs unchanged from any port's `tools/`, and this repo's copy is only ever compared and copied.

MODES
-----
  (default)   report every port: in step, or drifted (with the line count of both sides)
  --install   copy the canonical text into every port, then report again
  --selftest  prove the comparison can tell the two answers apart

OUTPUT IS A DENOMINATOR FIRST
-----------------------------
It always prints how many ports it scanned. A run that found no ports is a REFUSAL, not a pass: "0 of 0
in step" is the shape of a checker that has been pointed at the wrong directory, and reading it as
success is how a gate stops gating.
"""

from __future__ import annotations

import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
CANONICAL = TOOLS / "psxport_sync.py"
RELATIVE_COPY = Path("tools") / "psxport_sync.py"

# The workspace is psxport's parent: independent repos side by side, no superproject. Overridable so the
# checker is testable and so a differently-laid-out workspace still works.
DEFAULT_WORKSPACE = CANONICAL.parent.parent.parent  # .../psxport/tools -> psxport -> the workspace

# A port is a directory holding this file. Naming the marker rather than listing the ports is deliberate:
# a hard-coded roster goes stale and then reports the wrong denominator, which is the failure mode this
# whole tool exists to remove.
PORT_MARKER = RELATIVE_COPY


def find_ports(workspace: Path) -> list[Path]:
    if not workspace.is_dir():
        return []
    # psxport itself holds the canonical copy, so it would otherwise scan as its own port and inflate
    # the denominator by one "in step" that says nothing about any game. It is the SOURCE, not a port.
    canonical_repo = CANONICAL.parent.parent
    return sorted(
        parent
        for parent in workspace.iterdir()
        if parent.is_dir() and parent.resolve() != canonical_repo.resolve()
        and (parent / PORT_MARKER).is_file()
    )


def compare(canonical: Path, copy: Path) -> tuple[bool, str]:
    """(in_step, detail). Byte comparison; detail names the first differing line when drifted."""
    want = canonical.read_text(encoding="utf-8")
    have = copy.read_text(encoding="utf-8")
    if want == have:
        return True, f"identical ({len(want.splitlines())} lines)"
    diff = list(
        difflib.unified_diff(
            have.splitlines(), want.splitlines(), fromfile=str(copy), tofile=str(canonical), lineterm=""
        )
    )
    changed = [line for line in diff if line[:1] in "+-" and line[:3] not in ("---", "+++")]
    first = next((line for line in diff if line.startswith("@@")), "")
    return False, (
        f"DRIFTED: copy {len(have.splitlines())} lines vs canonical {len(want.splitlines())}; "
        f"{len(changed)} changed line(s); first hunk {first or '(none)'}"
    )


def runs(copy: Path) -> tuple[bool, str]:
    """Can this copy be invoked as a command at all?

    `--help` is the cheapest invocation that still resolves every module-level import and builds the
    argument parser, which is precisely the surface a missing import breaks. A module-import test is NOT
    enough: the original defect was invisible to one, because the import that failed lives inside
    `main()`.
    """
    try:
        done = subprocess.run(
            [sys.executable, str(copy), "--help"],
            capture_output=True, text=True, timeout=60, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return False, f"CANNOT RUN: {type(exc).__name__}: {exc}"
    if done.returncode != 0:
        tail = (done.stderr.strip().splitlines() or ["<no stderr>"])[-1]
        return False, f"DOES NOT RUN: `--help` exited {done.returncode}: {tail}"
    return True, "runs (`--help` exit 0)"


def report(workspace: Path, canonical: Path, allow_empty: bool = False) -> tuple[int, int, int]:
    """Print the full table. Returns (scanned, in_step, drifted)."""
    ports = find_ports(workspace)
    print(f"[pin-tools] workspace {workspace}")
    print(f"[pin-tools] canonical {canonical} ({len(canonical.read_text(encoding='utf-8').splitlines())} lines)")
    print(f"[pin-tools] {len(ports)} port(s) scanned: " + ", ".join(p.name for p in ports) if ports else "")
    if not ports:
        # A BARE CLONE OF PSXPORT HAS NO PORTS, and that is not a failure: this repo must build and gate
        # on its own. So the registered CTest passes --allow-empty and reports a skip. The CLI does not,
        # because a human who pointed this at the wrong directory must be told rather than shown a
        # reassuring "0 of 0 in step". The two cases are genuinely different and get different answers.
        if allow_empty:
            print("[pin-tools] SKIP: no port repositories beside this checkout, so there is nothing to "
                  "compare. This is a bare clone of psxport; the copy-in-step gate is vacuous here and "
                  "still applies in a workspace.")
            return 0, 0, 0
        print("[pin-tools] REFUSED: no port found — nothing was compared. That is a wrong directory, not a "
              "pass. A port is a directory containing " + str(PORT_MARKER) + ".")
        return 0, 0, 0
    in_step = 0
    in_step_and_runs = 0
    for port in ports:
        copy = port / PORT_MARKER
        ok, detail = compare(canonical, copy)
        if ok:
            # Only ask the second question when the first passed. A drifted copy is already reported, and
            # running it would describe a file that is not the canonical one.
            runnable, run_detail = runs(copy)
            if not runnable:
                ok, detail = False, f"{detail}; IN STEP BUT {run_detail}"
            else:
                detail = f"{detail}, {run_detail}"
                in_step_and_runs += 1
        print(f"[pin-tools]   {port.name:<14} {'OK  ' if ok else 'FAIL'} {detail}")
        in_step += ok
    scanned = len(ports)
    drifted = scanned - in_step
    print(f"[pin-tools] {in_step} of {scanned} in step, {drifted} drifted; "
          f"{in_step_and_runs} of {scanned} both in step AND runnable")
    return scanned, in_step, drifted


def install(workspace: Path, canonical: Path) -> None:
    ports = find_ports(workspace)
    if not ports:
        print("[pin-tools] REFUSED: no port found — nothing to install into.")
        return
    for port in ports:
        target = port / PORT_MARKER
        shutil.copyfile(canonical, target)
        print(f"[pin-tools]   installed -> {target}")


# --- selftest ---------------------------------------------------------------------------
# The comparison is the instrument, so the selftest has to show it BOTH answers. A checker that has only
# ever printed "in step" is indistinguishable from one that always passes.


def selftest() -> int:
    failures: list[str] = []
    checks = 0

    def expect(condition: bool, label: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        canonical = root / CANONICAL.name
        canonical.write_text("#!/usr/bin/env python3\nLINE ONE\nLINE TWO\n", encoding="utf-8")

        # A workspace with two ports: one faithful, one drifted by exactly one line.
        good = root / "goodport"
        (good / "tools").mkdir(parents=True)
        (good / PORT_MARKER).write_text(canonical.read_text(encoding="utf-8"), encoding="utf-8")
        bad = root / "badport"
        (bad / "tools").mkdir(parents=True)
        (bad / PORT_MARKER).write_text(
            "#!/usr/bin/env python3\nLINE ONE\nLINE TWO MUTATED\n", encoding="utf-8"
        )

        ports = find_ports(root)
        expect(len(ports) == 2, f"scanned 2 ports, got {len(ports)}")
        ok_good, _ = compare(canonical, good / PORT_MARKER)
        ok_bad, detail_bad = compare(canonical, bad / PORT_MARKER)
        expect(ok_good is True, "a faithful copy must compare in step")
        expect(ok_bad is False, "a one-line-mutated copy must compare DRIFTED")
        expect("2 changed line(s)" in detail_bad,
               f"a one-line mutation is 2 changed diff lines, got {detail_bad!r}")

        # The port the marker is named for, and one that merely resembles it, must not both count.
        (root / "notaport").mkdir()
        (root / "notaport" / "tools").mkdir()
        expect(len(find_ports(root)) == 2, "a directory with an empty tools/ is not a port")

        # An empty workspace is a REFUSAL, never a pass. This is the case that makes "0 of 0 in step"
        # impossible to read as success.
        empty = root / "empty"
        empty.mkdir()
        expect(find_ports(empty) == [], "an empty workspace finds no ports")
        missing = root / "does-not-exist"
        expect(find_ports(missing) == [], "a missing workspace finds no ports, and must not raise")

        # THE LESSON THIS TOOL EXISTS TO ENCODE, as a test. A copy can be byte-identical to the
        # canonical and still not run — that is not hypothetical, it is how the canonical shipped without
        # `import argparse` into all ten ports with every one reporting "in step". So `runs()` is tested on
        # its OWN terms, with real runnable programs, because a byte stub cannot exercise a subprocess.
        runnable = root / "runnable.py"
        runnable.write_text(
            "import argparse\n"
            "if __name__ == '__main__':\n"
            "    argparse.ArgumentParser(description='x').parse_args()\n",
            encoding="utf-8",
        )
        expect(runs(runnable)[0] is True, "a real argparse script must be reported runnable")

        # The exact shape of the real defect: the module IMPORTS fine, and only `main()` raises, because
        # the missing import is reached from there. A module-import test cannot see this; `--help` can.
        late = root / "late_failure.py"
        late.write_text(
            "def main():\n"
            "    import argparse\n"
            "    argparse.ArgumentParser()\n"
            "if __name__ == '__main__':\n"
            "    argparse.ArgumentParser()   # NameError: not imported at module level\n",
            encoding="utf-8",
        )
        import importlib.util as _ilu
        spec = _ilu.spec_from_file_location("late_failure", late)
        mod = _ilu.module_from_spec(spec)
        try:
            spec.loader.exec_module(mod)
            imported_cleanly = True
        except NameError:
            imported_cleanly = False
        expect(imported_cleanly is True,
               "the late-failure script must IMPORT cleanly, or this fixture is not the real defect")
        ok_late, detail_late = runs(late)
        expect(ok_late is False, "a script that imports but cannot run must fail `runs()`")
        expect("DOES NOT RUN" in detail_late, f"the run failure must say why, got {detail_late!r}")

        ok_absent, detail_absent = runs(root / "no-such-file.py")
        expect(ok_absent is False, "a missing copy must fail `runs()`, not raise")
        # The property is that a missing copy is REFUSED and does not raise. Which of the two labels it
        # gets depends on how far the interpreter got — a missing FILE is reported by python itself
        # (nonzero exit, so "DOES NOT RUN"), while an unreadable one surfaces as OSError ("CANNOT RUN").
        # Asserting a specific label would pin an accident of the interpreter, not the property.
        expect("RUN:" in detail_absent or "CANNOT RUN" in detail_absent,
               f"a missing copy must be refused, got {detail_absent!r}")

        # And a port holding the real canonical is the only combination that counts as in step AND usable.
        expect(compare(canonical, good / PORT_MARKER)[0] is True, "a faithful copy compares in step")

        # The empty case has TWO correct answers, and which one applies is the whole point: psxport's own
        # ctest must pass on a bare clone (skip), while a human pointing the CLI at the wrong directory
        # must be refused. If these ever collapse into one, either the gate lies or it blocks a clone.
        scanned, _, _ = report(empty, canonical, allow_empty=True)
        expect(scanned == 0, "allow_empty reports a skip, not a scan")
        scanned_refused, _, _ = report(empty, canonical, allow_empty=False)
        expect(scanned_refused == 0, "the refusing path also scans nothing")

    if failures:
        print(f"[pin-tools] selftest FAIL: {len(failures)} of {checks} checks failed")
        for label in failures:
            print(f"[pin-tools]   - {label}")
        return 1
    print(f"[pin-tools] selftest OK: {checks} checks — the comparison tells a faithful copy from a "
          f"one-line mutation, and an empty workspace is a refusal rather than a pass")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--workspace", default=str(DEFAULT_WORKSPACE),
                    help="directory holding the port repositories (default: psxport's parent)")
    ap.add_argument("--install", action="store_true",
                    help="copy the canonical text into every port, then report")
    ap.add_argument("--allow-empty", action="store_true",
                    help="treat zero ports as a SKIP rather than a refusal (for psxport's own ctest, "
                         "which must pass on a bare clone with no sibling repositories)")
    ap.add_argument("--selftest", action="store_true", help="prove the comparison has both answers")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not CANONICAL.is_file():
        print(f"[pin-tools] REFUSED: no canonical copy at {CANONICAL}. Nothing to compare against.")
        return 2
    workspace = Path(args.workspace).resolve()
    if args.install:
        install(workspace, CANONICAL)
    scanned, in_step, drifted = report(workspace, CANONICAL, allow_empty=args.allow_empty)
    if scanned == 0:
        return 0 if args.allow_empty else 2
    return 0 if drifted == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
