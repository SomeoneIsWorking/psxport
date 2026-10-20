"""The tool refusal type and process helpers shared by build and launch entry points."""

from __future__ import annotations

import os
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path


class ToolError(RuntimeError):
    """An actionable refusal at a tool boundary."""


def run(
    arguments: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path,
    environment: Mapping[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = [str(argument) for argument in arguments]
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        capture_output=capture,
        text=True,
    )
    if completed.returncode:
        detail = completed.stderr.strip() if capture else ""
        suffix = f": {detail}" if detail else ""
        raise ToolError(f"command failed ({completed.returncode}): {' '.join(command)}{suffix}")
    return completed


def cpu_jobs() -> int:
    """Build parallelism from the affinity mask, so a cgroup-limited host is not oversubscribed."""
    if hasattr(os, "sched_getaffinity"):
        return max(1, len(os.sched_getaffinity(0)))
    return max(1, os.cpu_count() or 1)


