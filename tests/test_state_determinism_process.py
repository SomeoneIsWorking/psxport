#!/usr/bin/env python3
"""Drive test_state_determinism's three phases and require the resumed machine to match the saver.

The comparison the C++ test cannot make for itself. Lightrec supports exactly one initialized machine
per process (measured, not assumed), so "the machine that saved" and "the machine that loaded" are two
OS processes with a state FILE between them. This driver runs all three phases and applies the two
verdicts that make the result mean something:

    save == load     the resumed machine continued exactly as the one that took the state
    save != control  the state file was actually consumed

The second is the one that stops the first from passing for the wrong reason. If the load were a no-op
and the synthetic program were somehow independent of the carried state, save and load would still
agree. The control phase runs the same phase-B program with no state loaded, and a resumed machine
that still agreed with it would mean the state is not influencing execution at all.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile

PHASES = ("save", "load", "control")

# The digest is the one 16-hex-digit line on stdout, and it is NOT reliably the last one: the executor
# prints its Lightrec RAM-usage line on stdout during shutdown, after the program has already written
# its result. Taking the last line — the obvious reading of "print the digest" — compares a digest
# against a memory-usage report, which is a string that differs between the saving machine and the
# resumed one and would report a divergence that is not one.
DIGEST_PATTERN = re.compile(r"^[0-9a-f]{16}$")


def run_phase(binary: pathlib.Path, phase: str, state_path: pathlib.Path) -> str:
    completed = subprocess.run(
        [str(binary), "--phase", phase, str(state_path)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        raise SystemExit(f"FAILED: phase '{phase}' exited {completed.returncode}")
    digests = [line.strip() for line in completed.stdout.splitlines() if DIGEST_PATTERN.match(line.strip())]
    if len(digests) != 1:
        raise SystemExit(
            f"FAILED: phase '{phase}' printed {len(digests)} digest line(s), expected exactly 1"
        )
    return digests[0]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <test_state_determinism binary>")
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise SystemExit(f"REFUSED: {binary} does not exist — build the target first")

    with tempfile.TemporaryDirectory(prefix="psxport-state-det-") as work:
        work_dir = pathlib.Path(work)
        state_path = work_dir / "det.state"
        digests = {phase: run_phase(binary, phase, state_path) for phase in PHASES}

        if not state_path.is_file():
            raise SystemExit("REFUSED: the save phase exited 0 but wrote no state file")
        size = state_path.stat().st_size
        print(f"[determinism] state file {size} bytes")
        for phase in PHASES:
            print(f"[determinism] {phase:<8} RAM digest {digests[phase]}")

        if digests["save"] != digests["load"]:
            print(
                "FAILED: the machine resumed from the state diverged from the machine that took it",
                file=sys.stderr,
            )
            return 1
        if digests["save"] == digests["control"]:
            print(
                "FAILED: the resumed machine matches one that never loaded the state, so the state is "
                "not influencing execution",
                file=sys.stderr,
            )
            return 1

    print("[determinism] OK: resumed machine continued exactly, and the state demonstrably mattered")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
