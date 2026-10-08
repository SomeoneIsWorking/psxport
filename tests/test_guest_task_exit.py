#!/usr/bin/env python3
"""Require unsupported guest-task faults to stop at their scheduler owner."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if os.name == "posix":
        import resource

        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run(
        [str(args.binary.resolve()), "--terminal-fault"],
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    output = result.stdout + result.stderr
    expected = "guest coroutine task required a completed guest call, but execution exited as fault at 0x80018000"
    if result.returncode == 0 or expected not in output:
        print(f"FAIL: expected scheduler-owned unsupported fault; exit={result.returncode}")
        print(output)
        return 1
    print("PASS: unsupported guest-task fault stopped at scheduler owner with exact reason and PC")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
