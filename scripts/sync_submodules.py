#!/usr/bin/env python3
"""Synchronize declared top-level submodules without clobbering deliberate work.

A NEW WORKTREE DOES NOT CLONE FROM THE NETWORK WHEN A SIBLING CHECKOUT ALREADY HAS THE COMMIT. A fresh
agent worktree of this repository has three declared submodules and no checkouts, so the sync used to
spend minutes fetching the same commits this machine already holds -- and the shared checkout it would
have fetched them from is frequently SHALLOW, which is exactly the case where a `--reference` clone
cannot borrow what it needs. :func:`submodule_state.local_sources` finds a checkout that already has
the recorded commit and :func:`submodule_state.initialize_from_local` clones from that path, then
restores the recorded URL.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

from submodule_state import (
    Git,
    Inventory,
    add_top_level_crosscheck,
    dirty_paths,
    enumerate_submodules,
    initialize_from_local,
    local_sources,
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
            # The local-source decision is made BEFORE the update and per path, so one submodule
            # served from a sibling checkout and one fetched from the network both work, and a
            # fallback is not all-or-nothing.
            overrides = os.environ.get("PSXPORT_SUBMODULE_SOURCES", "").split(os.pathsep)
            served: list[str] = []
            network: list[str] = []
            for item in inventory.uninitialized:
                sources = local_sources(git, root, item, overrides)
                if not sources:
                    network.append(item.path)
                    continue
                source, why = sources[0]
                say("cloning %s from a local checkout that already has %s (%s, %d more candidate(s))"
                    % (item.path, item.recorded[:10], why, len(sources) - 1))
                result = initialize_from_local(root, git, item, source)
                if result.returncode:
                    warn("local clone of %s from %s failed: %s" % (item.path, source,
                                                                  result.stderr.strip()))
                    network.append(item.path)
                    continue
                served.append(item.path)
            if not network:
                inventory = enumerate_submodules(root, git)
                add_top_level_crosscheck(root, git, inventory)
                say("cloned %d of %d uninitialized submodule(s) from a local checkout, 0 from the "
                    "network" % (len(served), len(served) + len(network)))
            else:
                say("fetching %d of %d submodule(s) from the recorded remote(s): %s"
                    % (len(network), len(served) + len(network), ", ".join(sorted(network))))
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
