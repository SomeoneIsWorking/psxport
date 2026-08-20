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
    "runtime/ui/render_path_control.cpp": 100,
    "runtime/ui/render_path_control.h": 50,
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

    # FORMAT-CHECK EVERY FIRST-PARTY SOURCE, not just the size-capped seams.
    #
    # USER, 2026-08-20: "apply clang format and use clang from now on ... you should accomodate to
    # the formatter not the other way around". Checking six files was how 280 first-party files came
    # to hold 56,735 violations while the gate stayed green — a gate that watches 2% of the tree
    # reports on 2% of the tree. The whole tree is formatted now, so this keeps it that way.
    #
    # vendor/ is excluded deliberately: it is third-party, and reformatting it would make every
    # future upstream diff unreadable.
    listed = subprocess.run(
        ["git", "ls-files", "*.cpp", "*.h", "*.c", "*.hpp"],
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout.split()
    sources = [ROOT / f for f in listed if not f.startswith("vendor/")]
    if not sources:
        print("cpp-style: REFUSED — git ls-files matched no first-party source; nothing was checked",
              file=sys.stderr)
        return 2

    formatted = subprocess.run(
        [formatter, "--dry-run", "--Werror", *map(str, sources)], cwd=ROOT, check=False
    )
    print(f"cpp-style: format-checked {len(sources)} first-party file(s) (vendor/ excluded)")
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
