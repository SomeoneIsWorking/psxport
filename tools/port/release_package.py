"""The platform-neutral half of a title's release package: its identity, its version and the content audit.

Every packager (the AppImage, the Windows archive) reads the title through here, so a package of any platform
carries the same name and version and refuses the same game content.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from port import desktop_shortcut

DISC_SUFFIXES = frozenset({".chd", ".cue", ".bin", ".iso", ".img", ".pbp", ".ccd", ".sub", ".mds", ".mdf"})
PSX_EXE_MAGIC = b"PS-X EXE"
ARTIFACT_NAME_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-")


class Mismatch(RuntimeError):
    """A readable input contradicts the package contract."""


class Refused(RuntimeError):
    """The package could not be built."""




@dataclass(frozen=True)
class Title:
    """The title's package identity, read from its committed desktop entry."""

    root: Path
    entry: desktop_shortcut.Shortcut
    name: str
    metainfo: Path | None
    licence: Path
    game_hashes: frozenset[str]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for block in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise Refused(f"cannot read {path}: {error}") from error


def game_hashes(root: Path) -> frozenset[str]:
    """The SHA-256 of every game file the title's manifests identify, so the audit can refuse them."""
    hashes: set[str] = set()
    for manifest in sorted((root / "titles").glob("*/*.json")):
        values = read_json(manifest)
        if not isinstance(values, dict):
            continue
        for key in ("sha256", "source_sha256"):
            value = values.get(key)
            if isinstance(value, str) and len(value) == 64:
                hashes.add(value.lower())
    return frozenset(hashes)


def resolve_title(root: Path) -> Title:
    try:
        entry = desktop_shortcut.resolve(root)
    except desktop_shortcut.ToolError as error:
        raise Refused(str(error)) from error
    name = desktop_shortcut.field(entry.source.read_text(encoding="utf-8"), "Name")
    artifact_name = "".join(c if c in ARTIFACT_NAME_CHARACTERS else "_" for c in name.replace(" ", "_")).strip("_")
    if not artifact_name:
        raise Refused(f"{entry.source} names no usable Name=")
    metainfo = entry.source.with_name(f"{entry.app_id}.metainfo.xml")
    licences = [root / candidate for candidate in ("LICENSE", "COPYING") if (root / candidate).is_file()]
    if not licences:
        raise Refused(f"the title has no LICENSE or COPYING at {root}")
    return Title(root=root,
                 entry=entry,
                 name=artifact_name,
                 metainfo=metainfo if metainfo.is_file() else None,
                 licence=licences[0],
                 game_hashes=game_hashes(root))




def git_value(root: Path, arguments: Sequence[str]) -> str:
    completed = subprocess.run(["git", *arguments], cwd=root, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise Refused(f"git {' '.join(arguments)} failed: "
                      f"{(completed.stderr or completed.stdout or 'no diagnostic').strip()}")
    return completed.stdout.strip()


def release_version(root: Path, requested: str | None, allow_dirty: bool) -> tuple[str, int]:
    if git_value(root, ["status", "--porcelain", "--untracked-files=no"]) and not allow_dirty:
        raise Refused("a release refuses a dirty worktree; land the change first")
    version = requested or git_value(root, ["describe", "--always", "--tags", "--dirty"])
    if not version or any(character not in ARTIFACT_NAME_CHARACTERS for character in version):
        raise Refused(f"version {version!r} contains a character that is unsafe in an artifact name")
    return version, int(git_value(root, ["show", "-s", "--format=%ct", "HEAD"]))


def run_checked(command: Sequence[str], *, environment: Mapping[str, str] | None = None,
                cwd: Path | None = None) -> None:
    completed = subprocess.run(list(command), text=True, capture_output=True, check=False, env=environment, cwd=cwd)
    if completed.returncode != 0:
        detail = "\n".join(output.strip() for output in (completed.stdout, completed.stderr) if output.strip())
        raise Refused(f"{' '.join(command)} failed:\n{detail or 'no diagnostic'}")


def audit_content(title: Title, tree: Path, label: str) -> int:
    """Refuses any disc image, PS-X executable or file the title's manifests identify as game content."""
    files = 0
    for path in tree.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        files += 1
        relative = path.relative_to(tree)
        if path.suffix.lower() in DISC_SUFFIXES:
            raise Mismatch(f"the {label} holds a disc-image-like file {relative}")
        with path.open("rb") as input_file:
            if input_file.read(len(PSX_EXE_MAGIC)) == PSX_EXE_MAGIC:
                raise Mismatch(f"the {label} holds a PS-X EXE at {relative}")
        if title.game_hashes and sha256(path) in title.game_hashes:
            raise Mismatch(f"the {label} holds identified game content at {relative}")
    if files == 0:
        raise Refused(f"the {label} audit scanned no files")
    return files
