#!/usr/bin/env python3
"""Remove an explicit allowlist of regenerable psxport artifacts."""

from __future__ import annotations

import sys
from pathlib import Path

from automation.cleanup import remove_explicit
from automation.process import ToolError

DIRECTORIES = (
    "build-duckstation",
    "scratch/frames",
    "scratch/gpustream",
    "scratch/screenshots",
    "scratch/logs",
    "scratch/raw",
    "scratch/wav",
    "scratch/spuperf",
    "scratch/fmvdev",
    "scratch/objdbg",
    "scratch/gpuobj",
    "scratch/gputest",
    "scratch/spudev",
    "scratch/mdecdev",
    "scratch/mdecfixdev",
    "scratch/cont",
)

GLOBS = ("scratch/agent-*", "scratch/*.o", "scratch/*.log", "scratch/shot*.png")


def targets(root: Path) -> list[Path]:
    result = [root / relative for relative in DIRECTORIES]
    for pattern in GLOBS:
        result.extend(root.glob(pattern))
    return result


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    try:
        removed, byte_count = remove_explicit(root, targets(root))
    except ToolError as error:
        print(f"clean: REFUSED: {error}", file=sys.stderr)
        return 2
    print(f"[clean] removed {removed} explicit artifact(s), freed approximately {byte_count // (1024 * 1024)} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
