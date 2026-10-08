"""Declared top-level submodule inventory and safe pin synchronization."""

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
    excluded_nested: list[str] = field(default_factory=list)

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
    return [path for _name, path in declared_entries(git, repo)]


def declared_entries(git: Git, repo: Path) -> list[tuple[str, str]]:
    """``(name, path)`` for every declared submodule, in .gitmodules order.

    The NAME is needed because the submodule's URL lives under ``submodule.<name>.url`` in the
    repository config, and pointing that at a local source is how a clone is redirected without
    editing .gitmodules -- which is shared by every worktree of the repository and is not ours to
    change.

    ``name`` is the BARE name (``vendor/beetle-psx``), not the config key the query returned
    (``submodule.vendor/beetle-psx.path``). MEASURED: returning the key with the ``submodule.``
    prefix left in it made the caller write ``submodule.submodule.vendor/beetle-psx.url``, so the
    override landed on a key nothing reads, the real URL stayed the GitHub one, and the clone
    silently fetched from the network -- a local clone that is not local and prints that it was.
    """
    modules = repo / ".gitmodules"
    if not modules.is_file():
        return []
    result = git.run(repo, ["config", "-f", str(modules), "--get-regexp", r"^submodule\..*\.path$"])
    if result.returncode not in (0, 1):
        raise RuntimeError(f"cannot read {modules}: {result.stderr.strip()}")
    entries: list[tuple[str, str]] = []
    for line in _lines(result.stdout):
        key, separator, path = line.partition(" ")
        if not separator or not key.startswith("submodule."):
            continue
        # `submodule.<name>.path` -> `<name>`, by removing BOTH the section prefix and the key.
        name = key[len("submodule."):].rsplit(".", 1)[0]
        entries.append((name, path))
    return entries


def worktree_paths(root: Path, git: Git) -> list[Path]:
    """Every other worktree of this repository, in the order ``git worktree list`` reports them.

    "Every other WORKTREE" rather than "the main checkout" on purpose: this repository's own scratch
    pins are worktrees too, and the main checkout is simply the one that has the submodule
    initialized. A name-based guess about which entry that is would break the first time a worktree
    is created for a branch rather than for a pin. Git lists the main checkout first, which is the
    one usually wanted; nothing DEPENDS on that, because :func:`local_sources` filters on the commit
    rather than on position.
    """
    result = git.run(root, ["worktree", "list", "--porcelain"])
    if result.returncode:
        raise RuntimeError(f"cannot list worktrees of {root}: {result.stderr.strip()}")
    paths: list[Path] = []
    for line in result.stdout.splitlines():
        if not line.startswith("worktree "):
            continue
        candidate = Path(line[len("worktree "):].strip())
        if candidate == root or candidate in paths:
            continue
        paths.append(candidate)
    return paths


def has_commit(repo: Path, git: Git, commit: str) -> bool:
    result = git.run(repo, ["cat-file", "-e", f"{commit}^{{commit}}"])
    return result.returncode == 0


def local_sources(git: Git, root: Path, item: Submodule,
                  extra: Sequence[str] | None = None) -> list[tuple[Path, str]]:
    """``(checkout, why)`` for every local repository that already HAS the recorded commit.

    The commit test is the whole filter. A local repository that does not contain the gitlink's
    commit cannot serve it, and handing its path to git would produce a clone that fails at checkout
    -- which reads as "the local clone is broken" rather than as "that repository has a different
    history", so the check happens first and the answer is a list of the ones that do.

    ``extra`` is the operator's override (``PSXPORT_SUBMODULE_SOURCES``, os.pathsep separated), tried
    before the worktree scan because a deliberate answer outranks a discovered one.
    """
    found: list[tuple[Path, str]] = []
    candidates: list[tuple[Path, str]] = []
    for raw in (extra or []):
        if raw.strip():
            candidates.append((Path(raw.strip()), "PSXPORT_SUBMODULE_SOURCES"))
    for worktree in worktree_paths(root, git):
        candidates.append((worktree / item.path, "worktree %s" % worktree))
    for checkout, why in candidates:
        if not (checkout / ".git").exists():
            continue
        if not has_commit(checkout, git, item.recorded):
            continue
        found.append((checkout, why))
    return found


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
    declared = _declared_paths(git, root)
    links = _gitlinks(git, root)
    declared_set = set(declared)
    inventory.unmanaged = sorted(path for path in links if path not in declared_set)

    for path in declared:
        recorded = links.get(path)
        if recorded is None:
            inventory.blind.append(BlindPath(path, "declared in .gitmodules but no gitlink in this repo's index"))
            continue

        checkout_root = root / path
        if not (checkout_root / ".git").exists():
            inventory.submodules.append(Submodule(path, recorded, None))
            inventory.blind.append(BlindPath(path, "top-level checkout is not initialized"))
            continue

        head = git.run(checkout_root, ["rev-parse", "HEAD"])
        if head.returncode or not head.stdout.strip():
            inventory.blind.append(BlindPath(path, "checkout exists but is not a readable git repo (HEAD unreadable)"))
            continue
        inventory.submodules.append(Submodule(path, recorded, head.stdout.strip()))
        inventory.excluded_nested.extend(f"{path}/{nested}" for nested in _gitlinks(git, checkout_root))

    inventory.unmanaged = sorted(set(inventory.unmanaged))
    inventory.excluded_nested = sorted(set(inventory.excluded_nested))
    return inventory


def add_top_level_crosscheck(root: Path, git: Git, inventory: Inventory) -> None:
    result = git.run(root, ["submodule", "status"])
    if result.returncode:
        raise RuntimeError(f"cannot cross-check top-level submodules: {result.stderr.strip()}")
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


def update_declared(root: Path, git: Git, paths: Iterable[str], *, initialize: bool) -> CommandResult:
    arguments = ["submodule", "update"]
    if initialize:
        arguments.append("--init")
    arguments.extend(["--", *sorted(paths)])
    return git.run(root, arguments)


def _submodule_name(git: Git, root: Path, path: str) -> str | None:
    for name, declared in declared_entries(git, root):
        if declared == path:
            return name
    return None


def initialize_from_local(root: Path, git: Git, item: Submodule, source: Path) -> CommandResult:
    """Clone ``item`` from ``source``, restoring the recorded URL afterwards.

    WHY A URL OVERRIDE AND NOT ``--reference``: ``git submodule update --init --reference DIR``
    still FETCHES from the configured remote and takes only what it cannot find locally, so it needs
    the network in exactly the case this exists for. And the shared checkout this reads is often
    SHALLOW, which is the case where ``--reference`` cannot help either -- a shallow repository has
    no objects for older commits to borrow, so the fetch is what fails. Cloning from a local path
    that is KNOWN to hold the commit needs neither the network nor history it does not have.

    ``protocol.file.allow=always`` is passed because every modern git refuses the ``file`` transport
    for a submodule clone by default (CVE-2022-39253, fixed in 2.38.1). The source is a directory on
    this machine that the caller named or that this repository's own worktree list vouches for, so
    the restriction this lifts is a defence against a repository that a third party supplied, not
    against anything this code fetched.

    The URL is restored on the way out even on failure. It is repository-local config, but it is
    SHARED by every worktree, and leaving it pointed at a scratch directory would make the next
    ``git submodule sync`` in another worktree resolve to a path that only exists while this one does.
    """
    name = _submodule_name(git, root, item.path)
    if name is None:
        return CommandResult(1, "", f"no .gitmodules entry for {item.path}")
    initialized = git.run(root, ["submodule", "init", "--", item.path])
    if initialized.returncode:
        return initialized
    key = f"submodule.{name}.url"
    previous = git.run(root, ["config", "--get", key])
    previous_url = previous.stdout.strip() if previous.returncode == 0 else None
    override = git.run(root, ["config", key, str(source)])
    if override.returncode:
        return override
    # The key is READ BACK and must equal the local source before the clone runs. A local clone that
    # silently fetches from the network is worse than no local clone at all: it prints the local
    # source and spends the minutes this exists to save. MEASURED: a wrong derivation of the
    # submodule NAME put the override on `submodule.submodule.<name>.url`, the real key kept the
    # GitHub URL, and git cloned from GitHub while the run reported a local source.
    applied = git.run(root, ["config", "--get", key])
    if applied.returncode or applied.stdout.strip() != str(source):
        return CommandResult(
            1, "", f"the URL override for {key} did not take (it reads "
                    f"{applied.stdout.strip() or 'nothing'}, expected {source}); refusing to run a "
                    "clone that would fetch from the recorded remote while reporting a local source")
    try:
        return git.run(root, ["-c", "protocol.file.allow=always", "submodule", "update", "--",
                              item.path])
    finally:
        if previous_url is None:
            git.run(root, ["config", "--unset", key])
        else:
            git.run(root, ["config", key, previous_url])
