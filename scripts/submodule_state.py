"""Complete, explicit submodule-state enumeration and safe pin synchronization."""

from __future__ import annotations

import subprocess
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


class Git:
    """Small injectable command boundary used by the synchronizer and its tests."""

    def __init__(self, environment: Mapping[str, str] | None = None) -> None:
        self._environment = environment

    def run(self, repo: Path, arguments: Sequence[str]) -> CommandResult:
        completed = subprocess.run(
            ["git", "-C", str(repo), *arguments],
            check=False,
            capture_output=True,
            text=True,
            env=self._environment,
        )
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)


@dataclass(frozen=True)
class Submodule:
    path: str
    recorded: str
    checkout: str | None


@dataclass(frozen=True)
class BlindPath:
    path: str
    reason: str


@dataclass
class Inventory:
    submodules: list[Submodule] = field(default_factory=list)
    blind: list[BlindPath] = field(default_factory=list)
    unmanaged: list[str] = field(default_factory=list)

    @property
    def declared_paths(self) -> list[str]:
        return sorted({item.path for item in self.submodules} | {item.path for item in self.blind})

    @property
    def resolved_paths(self) -> list[str]:
        return sorted(item.path for item in self.submodules if item.checkout is not None)

    @property
    def uninitialized(self) -> list[Submodule]:
        return [item for item in self.submodules if item.checkout is None]

    @property
    def off_pin(self) -> list[Submodule]:
        return [
            item
            for item in self.submodules
            if item.checkout is not None and item.checkout != item.recorded
        ]


def _lines(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines() if line.strip()]


def _declared_paths(git: Git, repo: Path) -> list[str]:
    modules = repo / ".gitmodules"
    if not modules.is_file():
        return []
    result = git.run(repo, ["config", "-f", str(modules), "--get-regexp", r"^submodule\..*\.path$"])
    if result.returncode not in (0, 1):
        raise RuntimeError(f"cannot read {modules}: {result.stderr.strip()}")
    return [line.split(None, 1)[1] for line in _lines(result.stdout) if len(line.split(None, 1)) == 2]


def _gitlinks(git: Git, repo: Path) -> dict[str, str]:
    result = git.run(repo, ["ls-files", "-s"])
    if result.returncode:
        raise RuntimeError(f"cannot inspect gitlinks in {repo}: {result.stderr.strip()}")
    links: dict[str, str] = {}
    for line in result.stdout.splitlines():
        metadata, separator, path = line.partition("\t")
        fields = metadata.split()
        if separator and len(fields) >= 2 and fields[0] == "160000":
            links[path] = fields[1]
    return links


def enumerate_submodules(root: Path, git: Git) -> Inventory:
    inventory = Inventory()

    def walk(repo: Path, prefix: str) -> None:
        declared = _declared_paths(git, repo)
        declared_set = set(declared)
        links = _gitlinks(git, repo)
        inventory.unmanaged.extend(prefix + path for path in links if path not in declared_set)

        for relative in declared:
            display = prefix + relative
            recorded = links.get(relative)
            if recorded is None:
                inventory.blind.append(
                    BlindPath(display, "declared in .gitmodules but no gitlink in this repo's index")
                )
                continue

            checkout_root = repo / relative
            if not (checkout_root / ".git").exists():
                inventory.submodules.append(Submodule(display, recorded, None))
                inventory.blind.append(
                    BlindPath(display, "not checked out — and nothing IT declares can be seen from here")
                )
                continue

            head = git.run(checkout_root, ["rev-parse", "HEAD"])
            if head.returncode or not head.stdout.strip():
                inventory.blind.append(
                    BlindPath(display, "checkout exists but is not a readable git repo (HEAD unreadable)")
                )
                continue
            inventory.submodules.append(Submodule(display, recorded, head.stdout.strip()))
            walk(checkout_root, display + "/")

    walk(root, "")
    inventory.unmanaged = sorted(set(inventory.unmanaged))
    return inventory


def add_recursive_crosscheck(root: Path, git: Git, inventory: Inventory) -> None:
    result = git.run(root, ["submodule", "status", "--recursive"])
    observed: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.lstrip("-+U ").split()
        if len(fields) >= 2:
            observed.add(fields[1])
    walked = {item.path for item in inventory.submodules}
    for path in sorted(observed - walked):
        inventory.blind.append(BlindPath(path, "listed by git's own recursion but MISSED by this walk"))


def dirty_paths(root: Path, git: Git, inventory: Inventory) -> list[str]:
    dirty: list[str] = []
    for item in inventory.submodules:
        if item.checkout is None:
            continue
        status = git.run(root / item.path, ["status", "--porcelain", "--ignore-submodules=all"])
        if status.returncode:
            raise RuntimeError(f"cannot inspect local work in {item.path}: {status.stderr.strip()}")
        if status.stdout.strip():
            dirty.append(item.path)
    return dirty


def protected_checkouts(root: Path, git: Git, items: Iterable[Submodule]) -> list[tuple[str, str]]:
    protected: list[tuple[str, str]] = []
    for item in items:
        if item.checkout is None or item.checkout == item.recorded:
            continue
        repo = root / item.path
        behind = git.run(repo, ["merge-base", "--is-ancestor", item.checkout, item.recorded])
        if behind.returncode == 0:
            continue
        ahead = git.run(repo, ["merge-base", "--is-ancestor", item.recorded, item.checkout])
        if ahead.returncode == 0:
            count = git.run(repo, ["rev-list", "--count", f"{item.recorded}..{item.checkout}"])
            amount = count.stdout.strip() if count.returncode == 0 else "?"
            protected.append(
                (item.path, f"AHEAD of the recorded gitlink by {amount} commit(s) — a deliberate checkout")
            )
        else:
            protected.append(
                (item.path, "DIVERGED from the recorded gitlink (neither is an ancestor of the other)")
            )
    return protected


def update_recursive(root: Path, git: Git, *, initialize: bool) -> CommandResult:
    arguments = ["submodule", "update"]
    if initialize:
        arguments.append("--init")
    arguments.append("--recursive")
    return git.run(root, arguments)
