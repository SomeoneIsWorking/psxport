"""Validated deletion primitives for explicitly scoped repository artifacts."""

from __future__ import annotations

import shutil
from collections.abc import Iterable
from pathlib import Path

from .process import ToolError


def contained_path(root: Path, candidate: Path) -> Path:
    resolved_root = root.resolve()
    resolved = candidate.resolve(strict=False)
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise ToolError(f"refusing path outside required root {resolved_root}: {candidate}")
    return resolved


def tree_size(path: Path) -> int:
    if path.is_file() or path.is_symlink():
        return path.lstat().st_size
    return sum(item.lstat().st_size for item in path.rglob("*") if item.is_file() or item.is_symlink())


def remove_explicit(root: Path, paths: Iterable[Path]) -> tuple[int, int]:
    removed = 0
    bytes_removed = 0
    for candidate in paths:
        path = contained_path(root, candidate)
        if not path.exists() and not path.is_symlink():
            continue
        bytes_removed += tree_size(path)
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()
        removed += 1
    return removed, bytes_removed
