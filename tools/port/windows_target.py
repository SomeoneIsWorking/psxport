"""The Windows x86-64 cross target: llvm-mingw, the target libraries and Lightning, built once per pin.

Everything lands under the shared dependency root (``PSXPORT_DEPENDENCY_ROOT``, else the user cache), keyed
by the pins that shape it, and is reused by every title's Windows build tree.
"""

from __future__ import annotations

import hashlib
import shutil
import tarfile
import urllib.request
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from automation.process import ToolError
from port import target_libraries
from psxport_fetch import pinned_revision

FRAMEWORK = Path(__file__).resolve().parents[2]
TOOLCHAIN_FILE = FRAMEWORK / "cmake" / "toolchains" / "llvm-mingw-x86_64.cmake"
TRIPLE = "x86_64-w64-mingw32"
LLVM_MINGW_RELEASE = "20260922"
LLVM_MINGW_NAME = f"llvm-mingw-{LLVM_MINGW_RELEASE}-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW_URL = (f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_RELEASE}/"
                  f"{LLVM_MINGW_NAME}.tar.xz")
LLVM_MINGW_SHA256 = "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21"


@dataclass(frozen=True)
class WindowsTarget:
    llvm_mingw: Path
    prefix: Path
    lightning: Path

    def cmake_definitions(self) -> list[str]:
        return [f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}", f"-DLLVM_MINGW_ROOT={self.llvm_mingw}",
                f"-DPSXPORT_TARGET_DEPENDENCY_PREFIX={self.prefix}", f"-DCMAKE_PREFIX_PATH={self.prefix}",
                f"-DCMAKE_FIND_ROOT_PATH={self.prefix}",
                # zlib's static MinGW name, which FindZLIB does not search.
                f"-DZLIB_LIBRARY={self.prefix / 'lib' / 'libzs.a'}",
                f"-DLIBLIGHTNING_INCLUDE_DIR={self.lightning / 'include'}",
                f"-DLIBLIGHTNING={self.lightning / 'lib' / 'liblightning.a'}"]


def dependency_root(environment: Mapping[str, str]) -> Path:
    """The same root cmake/dependency_prefix.cmake resolves."""
    if environment.get("PSXPORT_DEPENDENCY_ROOT"):
        return Path(environment["PSXPORT_DEPENDENCY_ROOT"])
    if environment.get("XDG_CACHE_HOME"):
        return Path(environment["XDG_CACHE_HOME"]) / "psxport" / "deps"
    return Path.home() / ".cache" / "psxport" / "deps"


def llvm_mingw(root: Path) -> Path:
    toolchain = root / LLVM_MINGW_NAME
    if (toolchain / "bin" / f"{TRIPLE}-clang").is_file():
        return toolchain
    downloads = root / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / f"{LLVM_MINGW_NAME}.tar.xz"
    if not archive.is_file():
        partial = archive.with_suffix(".part")
        with urllib.request.urlopen(LLVM_MINGW_URL) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out)
        partial.replace(archive)
    actual = target_libraries.sha256_of(archive)
    if actual != LLVM_MINGW_SHA256:
        raise ToolError(f"{LLVM_MINGW_URL}: sha256 {actual}, pinned {LLVM_MINGW_SHA256}")
    with tarfile.open(archive) as source:
        source.extractall(root, filter="data")
    archive.unlink()
    return toolchain


def _keyed(root: Path, kind: str, *parts: bytes) -> Path:
    digest = hashlib.sha256(LLVM_MINGW_NAME.encode())
    for part in parts:
        digest.update(part)
    return root / "windows-x86_64" / kind / digest.hexdigest()[:16]


def provision(environment: Mapping[str, str]) -> WindowsTarget:
    """The libraries and Lightning, each keyed by its own pins so a bump of one leaves the other."""
    root = dependency_root(environment)
    toolchain = llvm_mingw(root)
    compiler = str(toolchain / "bin" / f"{TRIPLE}-clang")
    lightning_revision = pinned_revision(str(FRAMEWORK), "LIGHTNING")
    if lightning_revision is None:
        raise ToolError("cmake/lightrec_dependency.cmake pins no Lightning revision")
    libraries = _keyed(root, "libraries", *(path.read_bytes() for path in
                                            (TOOLCHAIN_FILE, target_libraries.PINS, Path(target_libraries.__file__))))
    lightning = _keyed(root, "lightning", lightning_revision.encode())
    target = WindowsTarget(toolchain, libraries / "prefix", lightning / "lightning" / "build" / "install" / "windows-x86_64")
    try:
        if not (libraries / "complete").is_file():
            target_libraries.build_libraries("windows", target.prefix, libraries / "work",
                                             [f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}", f"-DLLVM_MINGW_ROOT={toolchain}",
                                              f"-DCMAKE_FIND_ROOT_PATH={target.prefix}"])
            shutil.rmtree(libraries / "work", ignore_errors=True)
            (libraries / "complete").touch()
        if not (lightning / "complete").is_file():
            target_libraries.build_lightning(lightning_revision, lightning, compiler, TRIPLE)
            (lightning / "complete").touch()
    except target_libraries.Refused as error:
        raise ToolError(f"Windows target dependencies: {error}") from error
    return target
