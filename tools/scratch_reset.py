#!/usr/bin/env python3
"""Empty named scratch subdirectories after validating their resolved scope."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from automation.cleanup import contained_path, remove_explicit
from automation.process import ToolError


def entry_count(root: Path) -> int:
    return sum(1 for _ in root.rglob("*")) if root.exists() else 0


def reset(repo: Path, argument: str) -> str:
    scratch = (repo / "scratch").resolve()
    candidate = Path(argument)
    if not candidate.is_absolute():
        candidate = repo / candidate
    target = candidate.resolve(strict=False)
    contained_path(scratch, target)
    if target.exists() and not target.is_dir():
        raise ToolError(f"target exists and is not a directory: {target}")
    if not target.exists():
        target.mkdir(parents=True)
        return f"scratch_reset: created {argument} (did not exist; 0 entries removed)"
    count = entry_count(target)
    remove_explicit(target, list(target.iterdir()))
    noun = "entry" if count == 1 else "entries"
    return f"scratch_reset: emptied {argument} ({count} {noun} removed)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", help="directories below the repository scratch root")
    arguments = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    try:
        for directory in arguments.directories:
            print(reset(repo, directory))
    except ToolError as error:
        print(f"scratch_reset: REFUSED: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
