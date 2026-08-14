#!/usr/bin/env python3
"""Enforce formatting and shrink-only size caps on framework C++ ownership seams."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# Legacy files start at their measured adoption size and may only shrink. New cohesive modules get
# deliberate headroom, not an invitation to become the next scheduler monolith.
FILE_CAPS = {
    "runtime/recomp/game_iface.h": 500,
    "runtime/recomp/pc_scheduler.cpp": 661,
    "runtime/recomp/pc_scheduler.h": 177,
    "runtime/recomp/synchronous_task_wait.cpp": 220,
    "runtime/recomp/synchronous_task_wait.h": 80,
    "tests/test_synchronous_task_wait.cpp": 180,
}


def main() -> int:
    formatter = shutil.which("clang-format")
    if formatter is None:
        print("cpp-style: REFUSED — clang-format is not installed", file=sys.stderr)
        return 2

    paths = [ROOT / relative for relative in FILE_CAPS]
    missing = [str(path.relative_to(ROOT)) for path in paths if not path.is_file()]
    if missing:
        print(f"cpp-style: REFUSED — missing managed file(s): {', '.join(missing)}", file=sys.stderr)
        return 2

    formatted = subprocess.run(
        [formatter, "--dry-run", "--Werror", *map(str, paths)], cwd=ROOT, check=False
    )
    if formatted.returncode:
        print("cpp-style: FAIL — run clang-format with the repository .clang-format", file=sys.stderr)
        return 1

    failed = False
    for relative, cap in FILE_CAPS.items():
        lines = len((ROOT / relative).read_text(encoding="utf-8").splitlines())
        print(f"cpp-style: {relative}: {lines}/{cap} lines")
        if lines > cap:
            print(
                f"cpp-style: FAIL — {relative} grew past its ownership cap; extract a cohesive module",
                file=sys.stderr,
            )
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
