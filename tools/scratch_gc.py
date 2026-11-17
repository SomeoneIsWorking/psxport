"""Delete named run leftovers from a repo's scratch/ directory, the one way agents sweep scratch.

    uv run --frozen python tools/scratch_gc.py ../psx-tekken-3/scratch/dynarec/*.trace [--dry-run]

Every path must lie inside some repo's `scratch/` (never `scratch/` itself), and none may sit in or contain a
subtree marked `.scratch-keep` or lie in a provisioned disc input docs/workspace/WORKSPACE.md names. A refused path stops the
whole sweep before anything is deleted. A directory goes with its contents; a symlink is removed, never followed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

KEEP = ".scratch-keep"
# The provisioned disc inputs WORKSPACE.md names, kept whether or not their marker was written: (repo, scratch dir).
PROVISIONED = {
    ("psx-spyro", "assets"),
    ("psx-tomba", "bin"),
    ("psx-toy-story-2", "bin"),
    ("psx-toy-story-2", "flat"),
    ("psx-toy-story-2", "raw"),
    ("psx-crash-bash", "bin"),
}


def refusal(path: Path) -> str | None:
    if not os.path.lexists(path):
        return "does not exist"
    absolute = Path(os.path.abspath(path))
    parts = absolute.parts
    if "scratch" not in parts:
        return "is not inside a scratch/ directory"
    scratch = Path(*parts[: len(parts) - parts[::-1].index("scratch")])
    if absolute == scratch:
        return "is scratch/ itself; name what to delete inside it"
    repo = scratch.parent.name
    inside = absolute.relative_to(scratch).parts
    if (repo, inside[0]) in PROVISIONED:
        return f"lies in {repo}/scratch/{inside[0]}, a provisioned disc input"
    for ancestor in [absolute, *absolute.parents]:
        if (ancestor / KEEP).exists():
            return f"lies under {ancestor / KEEP}"
        if ancestor == scratch:
            break
    if absolute.is_dir() and not absolute.is_symlink():
        for root, _dirs, files in os.walk(absolute):
            if KEEP in files:
                return f"contains {Path(root) / KEEP}"
    return None


def size(path: Path) -> int:
    if path.is_symlink() or not path.is_dir():
        return path.lstat().st_size
    return sum((Path(root) / name).lstat().st_size for root, _dirs, files in os.walk(path) for name in files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="list what would go, delete nothing")
    args = parser.parse_args()
    # One path named twice (overlapping globs) is deleted once.
    args.paths = list({os.path.abspath(path): path for path in args.paths}.values())
    refused = [(p, r) for p in args.paths if (r := refusal(p))]
    for path, reason in refused:
        print(f"refused: {path} {reason}", file=sys.stderr)
    if refused:
        return 1
    total = 0
    for path in args.paths:
        bytes_ = size(path)
        total += bytes_
        print(f"{'would delete' if args.dry_run else 'deleted'} {path} ({bytes_ / 1e6:.1f} MB)")
        if not args.dry_run:
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
    print(f"{'would free' if args.dry_run else 'freed'} {total / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
