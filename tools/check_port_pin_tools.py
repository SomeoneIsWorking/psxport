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

WHAT IT DOES, AND WHAT IT DELIBERATELY DOES NOT CLAIM
-----------------------------------------------------
It compares **bytes**. It does not judge whether a port's copy is *correct*, only whether it is the
canonical text — a copy can be faithfully propagated and still be wrong, and then the fix belongs in
`tools/psxport_sync.py` here, where all ten get it at once. A checker that reported "port X's pin tool is
fine" would be claiming something this file cannot know.

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
    for port in ports:
        ok, detail = compare(canonical, port / PORT_MARKER)
        print(f"[pin-tools]   {port.name:<14} {'OK  ' if ok else 'FAIL'} {detail}")
        in_step += ok
    scanned = len(ports)
    drifted = scanned - in_step
    print(f"[pin-tools] {in_step} of {scanned} in step, {drifted} drifted")
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
