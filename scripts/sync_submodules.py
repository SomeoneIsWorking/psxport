#!/usr/bin/env python3
"""Synchronize declared top-level submodules without clobbering deliberate work."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from submodule_state import (
    Git,
    Inventory,
    add_top_level_crosscheck,
    dirty_paths,
    enumerate_submodules,
    protected_checkouts,
    update_declared,
)


def say(message: str) -> None:
    print(f"[submodules] {message}")


def warn(message: str) -> None:
    print(f"[submodules] {message}", file=sys.stderr)


def coverage_note(inventory: Inventory) -> str:
    notes = []
    if inventory.unmanaged:
        notes.append("unmapped top-level gitlink(s) NOT covered: " + " ".join(inventory.unmanaged))
    if inventory.excluded_nested:
        notes.append("nested gitlink(s) outside this sync: " + " ".join(inventory.excluded_nested))
    return " — " + " — ".join(notes) if notes else ""


def enumerate_complete(root: Path, git: Git) -> Inventory:
    inventory = enumerate_submodules(root, git)
    add_top_level_crosscheck(root, git, inventory)
    return inventory


def report_blind(inventory: Inventory, *, after_sync: bool = False) -> int:
    prefix = "the sync left submodules this script can no longer see" if after_sync else (
        f"checked {len(inventory.resolved_paths)} of {len(inventory.declared_paths)} submodule(s)"
        f"{coverage_note(inventory)} — CANNOT SEE"
    )
    warn(prefix + ":")
    for item in inventory.blind:
        print(f"    {item.path}  ({item.reason})", file=sys.stderr)
    if not after_sync:
        warn("refusing to certify: this script cannot tell whether those are at their recorded gitlinks,")
        warn("and reporting 'all in sync' over a partial enumeration is the defect this check exists for.")
        warn("fix the listed top-level paths (usually: git submodule update --init -- <path>), then re-run.")
    return 1


def main() -> int:
    root = Path.cwd().resolve()
    if shutil.which("git") is None:
        say("git not found — skipping submodule sync")
        return 0
    if not (root / ".gitmodules").is_file():
        say("no .gitmodules here — nothing to sync")
        return 0

    git = Git()
    try:
        inventory = enumerate_submodules(root, git)
        if inventory.uninitialized:
            say("initializing declared top-level submodules…")
            result = update_declared(root, git, (item.path for item in inventory.uninitialized), initialize=True)
            inventory = enumerate_submodules(root, git)
            if result.returncode:
                warn(f"top-level initialization failed: {result.stderr.strip()}")
                if inventory.blind:
                    return report_blind(inventory)
                return 1
        add_top_level_crosscheck(root, git, inventory)
        if inventory.blind:
            return report_blind(inventory)

        denominator = len(inventory.declared_paths)
        note = coverage_note(inventory)
        if not inventory.off_pin:
            say(
                f"checked {denominator} of {denominator} submodule(s), "
                f"all at this repo's recorded gitlinks{note}"
            )
            return 0

        dirty = dirty_paths(root, git, inventory)
        if dirty:
            warn("NOT syncing — these submodules have uncommitted changes and a sync would discard them:")
            for path in dirty:
                print(f"    {path}", file=sys.stderr)
            warn("commit that work (the operator lands framework changes), then re-run.")
            warn("the build will use the CHECKED-OUT commits, which differ from this repo's recorded gitlinks.")
            return 0

        protected = protected_checkouts(root, git, inventory.off_pin)
        if protected:
            warn("NOT syncing — these submodules are not merely stale, and a sync would DISCARD a deliberate checkout:")
            for path, reason in protected:
                print(f"    {path}: {reason}", file=sys.stderr)
            warn("the build will use the CHECKED-OUT commits, not this repo's recorded gitlinks.")
            warn("if the checkout is what you want, RECORD it and the sync will move toward it instead:")
            warn("    git add <path> && git commit")
            warn("if you really want the recorded pin back: git submodule update -- <path>")
            return 0

        before = {item.path: item.checkout for item in inventory.submodules}
        result = update_declared(root, git, (item.path for item in inventory.off_pin), initialize=False)
        if result.returncode:
            warn(f"top-level update failed: {result.stderr.strip()}")
            return 1
        current = enumerate_complete(root, git)
        if current.blind:
            return report_blind(current, after_sync=True)

        moved = [
            item
            for item in current.submodules
            if before.get(item.path) is not None and before[item.path] != item.checkout
        ]
        if moved:
            say("synced submodules to this repo's recorded gitlinks:")
            for item in moved:
                print(f"    {item.path}: {before[item.path][:10]} -> {item.checkout[:10]}")
        if current.off_pin:
            warn("sync did NOT bring these to their recorded gitlinks:")
            for item in current.off_pin:
                print(f"    {item.path}", file=sys.stderr)
            warn("the build would use the wrong sources; fix these before building.")
            return 1
        if not moved:
            say(
                f"checked {denominator} of {denominator} submodule(s), "
                f"all at this repo's recorded gitlinks{note}"
            )
        return 0
    except RuntimeError as error:
        warn(f"REFUSED: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
