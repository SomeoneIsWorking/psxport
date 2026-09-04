"""Process helpers shared by build and maintenance entry points."""

from __future__ import annotations

import os
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path


class ToolError(RuntimeError):
    """An actionable refusal at a tool boundary."""


def require_program(name: str, guidance: str | None = None) -> str:
    path = shutil.which(name)
    if path is None:
        suffix = f" ({guidance})" if guidance else ""
        raise ToolError(f"required program not found: {name}{suffix}")
    return path


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
    return max(1, os.cpu_count() or 1)


def first_file(root: Path, filename: str) -> Path | None:
    return next((path for path in sorted(root.rglob(filename)) if path.is_file()), None)


def repository_root(script: Path, levels: int = 1) -> Path:
    return script.resolve().parents[levels]
