"""The tool refusal type and the build parallelism shared by build and launch entry points."""

from __future__ import annotations

import os


class ToolError(RuntimeError):
    """An actionable refusal at a tool boundary."""


def cpu_jobs() -> int:
    """Build parallelism from the affinity mask, so a cgroup-limited host is not oversubscribed."""
    if hasattr(os, "sched_getaffinity"):
        return max(1, len(os.sched_getaffinity(0)))
    return max(1, os.cpu_count() or 1)
