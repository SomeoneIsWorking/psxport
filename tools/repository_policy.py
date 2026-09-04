"""Whole-tree first-party source policy checks."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

EXCLUDED_TOP_LEVEL = {"build", "external", "scratch", "third_party", "vendor"}
_FORBIDDEN_TEMPLATE_NAME = "Dusk" + "light"
_STALE_EXECUTION_TERMS = (
    "emit" + ".py",
    "abi_" + "extract.py",
    "port_" + "gen.py",
    "port_" + "check.py",
    "psx_port_" + "scaffold.py",
    "sym" + "res.py",
    "tools/decomp" + ".sh",
    "GenericWholeProgram" + "Profile",
    "guest " + "substrate",
    "generated guest " + "body",
    "generated guest " + "source",
    "emitted whole-" + "program",
    "re-emitted " + "guest",
    "Backend" + "Unavailable",
    "currently bundled " + "Lightrec",
    "unavailable initialization may " + "enter",
)
_DELETED_PATHS = (
    "runtime/" + "recomp",
    "tools/" + "recomp",
    "scripts/bootstrap" + "-workspace.sh",
    "scripts/build" + "-openbios.sh",
    "scripts/sync" + "-submodules.sh",
    "tools/build" + "_rmlui.sh",
    "tools/build" + "_xa_wavdump.sh",
    "tools/" + "clean.sh",
    "tools/fmv_compare/" + "build.sh",
    "tools/fmv_export/" + "build.sh",
    "tools/" + "scratch_reset.sh",
    "tools/" + "syntaxcheck.sh",
    "shared/" + "jit-common",
    "runtime/cpu/" + "state_bridge",
    "runtime/cpu/" + "code_identity",
)


def first_party_files(root: Path) -> list[Path]:
    """Return tracked and untracked first-party files, excluding dependency/output trees."""
    commands = (
        ["git", "-C", str(root), "ls-files", "-z"],
        ["git", "-C", str(root), "ls-files", "--others", "--exclude-standard", "-z"],
    )
    relative_paths: set[str] = set()
    for command in commands:
        result = subprocess.run(command, check=True, capture_output=True)
        relative_paths.update(
            item.decode("utf-8", errors="surrogateescape")
            for item in result.stdout.split(b"\0")
            if item
        )

    files: list[Path] = []
    for relative in sorted(relative_paths):
        parts = Path(relative).parts
        path = root / relative
        if parts and parts[0] not in EXCLUDED_TOP_LEVEL and path.is_file():
            files.append(path)
    return files


def forbidden_architecture_references(root: Path) -> list[str]:
    """Find references that make another game's tree an architecture authority."""
    needle = _FORBIDDEN_TEMPLATE_NAME.casefold().encode("ascii")
    findings: list[str] = []
    for path in first_party_files(root):
        try:
            content = path.read_bytes().lower()
        except OSError as exc:
            raise RuntimeError(f"cannot inspect first-party file {path}: {exc}") from exc
        if needle in content:
            findings.append(str(path.relative_to(root)))
    return findings


def deleted_path_references(root: Path) -> list[str]:
    """Find references to paths removed by the framework migration."""
    needles = tuple(path.casefold().encode("ascii") for path in _DELETED_PATHS)
    findings: list[str] = []
    for path in first_party_files(root):
        try:
            content = path.read_bytes().lower()
        except OSError as exc:
            raise RuntimeError(f"cannot inspect first-party file {path}: {exc}") from exc
        if any(needle in content for needle in needles):
            findings.append(str(path.relative_to(root)))
    return findings


def stale_execution_references(root: Path) -> list[str]:
    """Find retired guest-generation and missing-backend-as-fallback vocabulary."""
    needles = tuple(term.casefold().encode("utf-8") for term in _STALE_EXECUTION_TERMS)
    findings: list[str] = []
    for path in first_party_files(root):
        relative = path.relative_to(root).as_posix()
        if relative in {"tools/repository_policy.py", "tools/check_execution_boundary.py"}:
            continue
        try:
            content = path.read_bytes().lower()
        except OSError as exc:
            raise RuntimeError(f"cannot inspect first-party file {path}: {exc}") from exc
        if any(needle in content for needle in needles):
            findings.append(relative)
    return findings


def _code_without_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def product_boundary_bypasses(root: Path) -> list[str]:
    """Find direct product diagnostic/configuration boundary bypasses."""
    stderr_patterns = (
        re.compile(r"\bstd::cerr\b"),
        re.compile(r"\bfprintf\s*\(\s*stderr\b"),
        re.compile(r"\bfputc\s*\([^\n;]*\bstderr\b"),
        re.compile(r"\bfflush\s*\(\s*stderr\b"),
        re.compile(r"\bwrite\s*\(\s*2\s*,"),
    )
    environment_pattern = re.compile(r"\b(?:std::)?getenv\s*\(")
    findings: list[str] = []
    for path in first_party_files(root):
        relative = path.relative_to(root).as_posix()
        if not relative.startswith(("runtime/cpu/", "runtime/psx/", "runtime/ui/")):
            continue
        if relative == "runtime/psx/watchdog.cpp":
            continue
        try:
            source = _code_without_comments(path.read_text(encoding="utf-8", errors="replace"))
        except OSError as exc:
            raise RuntimeError(f"cannot inspect first-party file {path}: {exc}") from exc
        if any(pattern.search(source) for pattern in stderr_patterns):
            findings.append(f"{relative}: direct diagnostic sink")
        if environment_pattern.search(source):
            findings.append(f"{relative}: direct process environment")
    return findings
