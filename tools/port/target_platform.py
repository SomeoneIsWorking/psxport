"""The release platforms a title builds for beside the Linux host: their toolchain, target libraries and Lightning.

Windows x86-64 cross-compiles with llvm-mingw; macOS x86-64 builds natively on an Intel Mac. Each platform's
libraries (target-libraries.json) and Lightning land under the shared dependency root
(``PSXPORT_DEPENDENCY_ROOT``, else the user cache), keyed by the pins that shape them, and are reused by every
title's build tree. MoltenVK, which a macOS package carries, is pinned here too.
"""

from __future__ import annotations

import hashlib
import shutil
import tarfile
import urllib.request
from collections.abc import Mapping
from pathlib import Path

from automation.process import ToolError
from port import target_libraries
from psxport_fetch import pinned_revision

FRAMEWORK = Path(__file__).resolve().parents[2]
WINDOWS_TOOLCHAIN_FILE = FRAMEWORK / "cmake" / "toolchains" / "llvm-mingw-x86_64.cmake"
WINDOWS_TRIPLE = "x86_64-w64-mingw32"
LLVM_MINGW_RELEASE = "20260922"
LLVM_MINGW_NAME = f"llvm-mingw-{LLVM_MINGW_RELEASE}-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW_URL = (f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_RELEASE}/"
                  f"{LLVM_MINGW_NAME}.tar.xz")
LLVM_MINGW_SHA256 = "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21"
# The oldest macOS the package runs on: std::format of floating point needs 13.3.
MACOS_DEPLOYMENT_TARGET = "13.3"
MOLTENVK_RELEASE = "v1.4.2"
MOLTENVK_URL = f"https://github.com/KhronosGroup/MoltenVK/releases/download/{MOLTENVK_RELEASE}/MoltenVK-macos.tar"
MOLTENVK_SHA256 = "f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"
PLATFORMS = ("windows", "macos")


def dependency_root(environment: Mapping[str, str]) -> Path:
    """The same root cmake/dependency_prefix.cmake resolves."""
    if environment.get("PSXPORT_DEPENDENCY_ROOT"):
        return Path(environment["PSXPORT_DEPENDENCY_ROOT"])
    if environment.get("XDG_CACHE_HOME"):
        return Path(environment["XDG_CACHE_HOME"]) / "psxport" / "deps"
    return Path.home() / ".cache" / "psxport" / "deps"


def _download(url: str, sha256: str, root: Path, name: str) -> Path:
    downloads = root / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / name
    if not archive.is_file():
        partial = archive.with_suffix(".part")
        with urllib.request.urlopen(url) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out)
        partial.replace(archive)
    actual = target_libraries.sha256_of(archive)
    if actual != sha256:
        raise ToolError(f"{url}: sha256 {actual}, pinned {sha256}")
    return archive


def _extracted(url: str, sha256: str, root: Path, directory: str, marker: Path) -> Path:
    """Extract a pinned archive into ``root`` once; ``marker`` (relative to ``root / directory``) proves it."""
    if (root / directory / marker).exists():
        return root / directory
    archive = _download(url, sha256, root, url.rsplit("/", 1)[1])
    with tarfile.open(archive) as source:
        source.extractall(root, filter="data")
    archive.unlink()
    if not (root / directory / marker).exists():
        raise ToolError(f"{url} did not hold {directory}/{marker}")
    return root / directory


def llvm_mingw(root: Path) -> Path:
    return _extracted(LLVM_MINGW_URL, LLVM_MINGW_SHA256, root, LLVM_MINGW_NAME,
                      Path("bin") / f"{WINDOWS_TRIPLE}-clang")


def moltenvk(root: Path) -> Path:
    """The pinned MoltenVK dylib a macOS package carries."""
    library = Path("MoltenVK") / "dynamic" / "dylib" / "macOS" / "libMoltenVK.dylib"
    return _extracted(MOLTENVK_URL, MOLTENVK_SHA256, root / f"moltenvk-{MOLTENVK_RELEASE}", "MoltenVK",
                      library) / library


def _keyed(root: Path, platform: str, kind: str, *parts: bytes) -> Path:
    digest = hashlib.sha256(platform.encode())
    for part in parts:
        digest.update(part)
    return root / f"{platform}-x86_64" / kind / digest.hexdigest()[:16]


def provision(platform: str, environment: Mapping[str, str]) -> list[str]:
    """Build the platform's libraries and Lightning when missing and return the CMake definitions that use them."""
    if platform not in PLATFORMS:
        raise ToolError(f"no target platform {platform!r}; known: {', '.join(PLATFORMS)}")
    root = dependency_root(environment)
    lightning_revision = pinned_revision(str(FRAMEWORK), "LIGHTNING")
    if lightning_revision is None:
        raise ToolError("cmake/lightrec_dependency.cmake pins no Lightning revision")
    if platform == "windows":
        toolchain = llvm_mingw(root)
        library_arguments = [f"-DCMAKE_TOOLCHAIN_FILE={WINDOWS_TOOLCHAIN_FILE}", f"-DLLVM_MINGW_ROOT={toolchain}"]
        shaping = (LLVM_MINGW_NAME.encode(), WINDOWS_TOOLCHAIN_FILE.read_bytes())
        compiler, host = str(toolchain / "bin" / f"{WINDOWS_TRIPLE}-clang"), WINDOWS_TRIPLE
    else:
        library_arguments = [f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_DEPLOYMENT_TARGET}"]
        shaping = (MACOS_DEPLOYMENT_TARGET.encode(),)
        compiler, host = "clang", None
    libraries = _keyed(root, platform, "libraries", *shaping, target_libraries.PINS.read_bytes(),
                       Path(target_libraries.__file__).read_bytes())
    lightning = _keyed(root, platform, "lightning", *shaping, lightning_revision.encode())
    prefix = libraries / "prefix"
    profile = "windows-x86_64" if platform == "windows" else "macos-x86_64"
    lightning_install = lightning / "lightning" / "build" / "install" / profile
    try:
        if not (libraries / "complete").is_file():
            target_libraries.build_libraries(platform, prefix, libraries / "work",
                                             [*library_arguments, f"-DCMAKE_FIND_ROOT_PATH={prefix}"])
            shutil.rmtree(libraries / "work", ignore_errors=True)
            (libraries / "complete").touch()
        if not (lightning / "complete").is_file():
            target_libraries.build_lightning(lightning_revision, lightning, compiler, host)
            (lightning / "complete").touch()
    except target_libraries.Refused as error:
        raise ToolError(f"{platform} target dependencies: {error}") from error
    definitions = [*library_arguments, f"-DCMAKE_PREFIX_PATH={prefix}",
                   f"-DLIBLIGHTNING_INCLUDE_DIR={lightning_install / 'include'}",
                   f"-DLIBLIGHTNING={lightning_install / 'lib' / 'liblightning.a'}"]
    if platform == "windows":
        definitions += [f"-DPSXPORT_TARGET_DEPENDENCY_PREFIX={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
                        # zlib's static MinGW name, which FindZLIB does not search.
                        f"-DZLIB_LIBRARY={prefix / 'lib' / 'libzs.a'}"]
    return definitions
