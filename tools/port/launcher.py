"""The title launcher: native dependency preflight, the consumer CMake configure and build, the exec.

A title's `tools/run.py` keeps only its own data (product target, extra dependencies, provisioning
and launch arguments) and composes these. Its `bootstrap.py` fetches `external/psxport` first and
puts this checkout's `tools/` on `sys.path`; a refusal is an `automation.process.ToolError`.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import psxport_fetch  # noqa: E402
from automation.process import ToolError, cpu_jobs  # noqa: E402
from project import lightning_cmake_definitions, lightrec_cmake_definitions  # noqa: E402

FRAMEWORK = TOOLS.parent
PLAYER_BUILD = Path("build/player")
FRAMEWORK_TOOLS_BUILD = Path("build/player-tools")
CMAKE_STATE = ("CMakeCache.txt", "CMakeFiles", "build.ninja", "rules.ninja", ".ninja_deps", ".ninja_log",
               "cmake_install.cmake", "CPackConfig.cmake", "CPackSourceConfig.cmake")


def say(message: str) -> None:
    print(f"[run] {message}", file=sys.stderr, flush=True)


@dataclass(frozen=True)
class Dependency:
    """A native build prerequisite: a program on PATH or a pkg-config module, with install commands."""

    label: str
    install: Mapping[str, str]
    tool: str | None = None
    pkg_config: str | None = None


def install_commands(fedora: str, debian: str, macos: str, windows: str) -> dict[str, str]:
    return {"fedora": f"sudo dnf install {fedora}", "debian": f"sudo apt install {debian}",
            "macos": macos, "windows": windows}


CMAKE = Dependency("CMake", install_commands("cmake", "cmake", "brew install cmake", "winget install Kitware.CMake"),
                   tool="cmake")
NINJA = Dependency("Ninja", install_commands("ninja-build", "ninja-build", "brew install ninja",
                                             "winget install Ninja-build.Ninja"), tool="ninja")
GIT = Dependency("Git", install_commands("git", "git", "xcode-select --install", "winget install Git.Git"), tool="git")
PKG_CONFIG = Dependency("pkg-config", install_commands("pkgconf-pkg-config", "pkg-config", "brew install pkg-config",
                                                       "vcpkg install pkgconf"), tool="pkg-config")
GLSLC = Dependency("glslc (Vulkan shader compiler)",
                   install_commands("glslc", "glslc", "brew install shaderc", "winget install KhronosGroup.VulkanSDK"),
                   tool="glslc")
SDL3 = Dependency("SDL3 development files",
                  install_commands("SDL3-devel", "libsdl3-dev", "brew install sdl3", "vcpkg install sdl3"),
                  pkg_config="sdl3")
SDL3_IMAGE = Dependency("SDL3_image development files",
                        install_commands("SDL3_image-devel", "libsdl3-image-dev", "brew install sdl3_image",
                                         "vcpkg install sdl3-image"), pkg_config="sdl3-image")
FREETYPE = Dependency("FreeType development files",
                      install_commands("freetype-devel", "libfreetype-dev", "brew install freetype",
                                       "vcpkg install freetype"), pkg_config="freetype2")
ZLIB = Dependency("zlib development files",
                  install_commands("zlib-devel", "zlib1g-dev", "brew install zlib", "vcpkg install zlib"),
                  pkg_config="zlib")
ZSTD = Dependency("zstd development files",
                  install_commands("libzstd-devel", "libzstd-dev", "brew install zstd", "vcpkg install zstd"),
                  pkg_config="libzstd")
COMPILER = install_commands("clang", "clang", "xcode-select --install", "winget install LLVM.LLVM")

# What psxport itself needs to configure and build.
FRAMEWORK_DEPENDENCIES = (CMAKE, NINJA, GIT, PKG_CONFIG, GLSLC, SDL3, SDL3_IMAGE, FREETYPE, ZLIB, ZSTD)


def host_family() -> str:
    """fedora | debian | macos | windows | unknown: the key into a dependency's install commands."""
    system = platform.system()
    if system == "Darwin":
        return "macos"
    if system == "Windows":
        return "windows"
    if system != "Linux":
        return "unknown"
    identities: set[str] = set()
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8", errors="replace").splitlines():
            key, separator, value = line.partition("=")
            if separator and key in ("ID", "ID_LIKE"):
                identities.update(value.strip().strip('"').lower().split())
    except OSError:
        return "unknown"
    if identities & {"fedora", "rhel", "centos", "rocky", "almalinux"}:
        return "fedora"
    if identities & {"debian", "ubuntu", "linuxmint", "pop"}:
        return "debian"
    return "unknown"


def install_instruction(install: Mapping[str, str], label: str) -> str:
    command = install.get(host_family())
    if command:
        return f"install it with: {command}"
    return (f"no install command is recorded for {platform.system()} {platform.release()}; install "
            f"{label} with your package manager and report the platform")


def _present(dependency: Dependency) -> bool:
    if dependency.tool is not None:
        return shutil.which(dependency.tool) is not None
    if shutil.which("pkg-config") is None:
        return False
    return subprocess.run(["pkg-config", "--exists", dependency.pkg_config], check=False).returncode == 0


def require(dependencies: Iterable[Dependency]) -> None:
    """Refuse, naming every missing dependency and its install command."""
    missing = [d for d in dependencies if not _present(d)]
    if missing:
        raise ToolError("missing native dependencies:\n" + "\n".join(
            f"  {d.label}: {install_instruction(d.install, d.label)}" for d in missing))


def resolved_compiler(requested: str) -> str:
    """The path CMake will cache for a bare compiler name, so a respelling never looks like a switch."""
    if os.sep in requested or (os.altsep and os.altsep in requested):
        return requested
    return shutil.which(requested) or requested


def compiler_definitions(environment: Mapping[str, str]) -> list[str]:
    """CC/CXX when set, else Clang when installed, else CMake's own choice of cc/c++."""
    chosen = [(env, cmake, environment[env]) for env, cmake in (("CC", "CMAKE_C_COMPILER"),
                                                                ("CXX", "CMAKE_CXX_COMPILER"))
              if environment.get(env)]
    if not chosen and shutil.which("clang") and shutil.which("clang++"):
        chosen = [("CC", "CMAKE_C_COMPILER", "clang"), ("CXX", "CMAKE_CXX_COMPILER", "clang++")]
    for env, _cmake, compiler in chosen:
        if shutil.which(compiler) is None:
            raise ToolError(f"{env}={compiler} is not an executable compiler; "
                            f"{install_instruction(COMPILER, 'a C/C++ compiler')}")
    if not chosen and not (shutil.which("cc") and shutil.which("c++")):
        raise ToolError(f"no C/C++ compiler found; {install_instruction(COMPILER, 'a C/C++ compiler')}")
    return [f"-D{cmake}={resolved_compiler(compiler)}" for _env, cmake, compiler in chosen]


def _cache_value(build: Path, key: str) -> str:
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    prefix = f"{key}:"
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith(prefix):
            return line.partition("=")[2]
    return ""


def _same_compiler(left: str, right: str) -> bool:
    """Equal absolute paths, or equal basenames when either is bare; symlinks are not followed (ccache)."""
    left_path, right_path = Path(left), Path(right)
    if left_path.is_absolute() and right_path.is_absolute():
        return left_path == right_path
    return left_path.name == right_path.name


def _compiler_mismatch(build: Path, definitions: Sequence[str]) -> str:
    for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
        requested = next((d.partition("=")[2] for d in definitions if d.startswith(f"-D{key}=")), "")
        recorded = _cache_value(build, key)
        if requested and recorded and not _same_compiler(requested, recorded):
            return f"{key} is {recorded}, requested {requested}"
    return ""


def _reset_cmake_state(build: Path) -> None:
    targets = [build / name for name in CMAKE_STATE]
    if any(target.is_symlink() for target in targets):
        raise ToolError(f"refusing symlinked CMake state in {build}")
    for target in targets:
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink(missing_ok=True)


def configure(source: Path, build: Path, build_type: str, definitions: Sequence[str],
              environment: Mapping[str, str], *, owner: Path | None = None, quiet: bool = False) -> None:
    """One Ninja configure of ``source`` into ``build`` with the chosen compiler.

    ``build`` must be a real path under ``owner/build`` (``owner`` defaults to ``source``). A tree
    whose cached source, generator or compiler disagrees is reset first: CMake's own re-run after a
    compiler change does not keep the requested compiler.
    """
    source = source.resolve()
    owner = (owner or source).resolve()
    build = build.absolute()
    if not build.is_relative_to(owner / "build") or build.resolve() != build:
        raise ToolError(f"build directory must be a real path under {owner / 'build'}: {build}")
    options = [*compiler_definitions(environment), f"-DCMAKE_BUILD_TYPE={build_type}",
               f"-DPython3_EXECUTABLE={sys.executable}", *definitions]
    cached_source = _cache_value(build, "CMAKE_HOME_DIRECTORY")
    generator = _cache_value(build, "CMAKE_GENERATOR")
    stale = ((cached_source and Path(cached_source).resolve() != source and "the source directory")
             or (generator and generator != "Ninja" and "the generator")
             or _compiler_mismatch(build, options))
    if stale:
        say(f"resetting CMake state in {build} for {stale}")
        _reset_cmake_state(build)
    build.mkdir(parents=True, exist_ok=True)
    run(["cmake", "-S", source, "-B", build, "-G", "Ninja", *options], environment, quiet=quiet)
    mismatch = _compiler_mismatch(build, options)
    if mismatch:
        raise ToolError(f"CMake configured a different compiler than requested: {mismatch}")


def build_targets(build: Path, targets: Sequence[str], environment: Mapping[str, str]) -> None:
    command = ["cmake", "--build", build, "--parallel", str(cpu_jobs())]
    for target in targets:
        command += ["--target", target]
    run(command, environment)


def framework_definitions(environment: Mapping[str, str]) -> list[str]:
    """``-DPSXPORT_DIR`` and the Lightrec/Lightning overrides a consumer configure passes."""
    return [f"-DPSXPORT_DIR={FRAMEWORK}", *lightrec_cmake_definitions(environment),
            *lightning_cmake_definitions(environment)]


def configure_consumer(root: Path, build: Path, build_type: str, definitions: Sequence[str],
                       environment: Mapping[str, str], *, quiet: bool = False) -> None:
    """Configure a title repository against this framework checkout."""
    configure(root, build, build_type, [*framework_definitions(environment), *definitions], environment,
              quiet=quiet)


def run(command: Sequence[object], environment: Mapping[str, str], *, cwd: Path | None = None,
        quiet: bool = False) -> None:
    """Run one launcher step, refusing on a non-zero exit."""
    arguments = [str(value) for value in command]
    try:
        completed = subprocess.run(arguments, env=dict(environment), cwd=cwd, check=False,
                                   stdout=subprocess.DEVNULL if quiet else None)
    except OSError as error:
        raise ToolError(f"could not run {arguments[0]}: {error}") from error
    if completed.returncode:
        raise ToolError(f"command failed ({completed.returncode}): {' '.join(arguments)}")


def _git(*arguments: str) -> str:
    done = subprocess.run(["git", "-C", str(FRAMEWORK.resolve()), *arguments], capture_output=True,
                          text=True, check=False)
    return done.stdout.strip() if done.returncode == 0 else ""


def preflight(extra: Iterable[Dependency] = ()) -> None:
    """Refuse on a missing dependency, name the framework checkout, and initialise its vendors."""
    require((*FRAMEWORK_DEPENDENCIES, *extra))
    dirty = " +dirty" if _git("status", "--porcelain") else ""
    say(f"framework: {FRAMEWORK.resolve()} @ {_git('rev-parse', '--short', 'HEAD') or '?'}{dirty}")
    psxport_fetch.init_submodules(str(FRAMEWORK.resolve()))


def build_product(root: Path, target: str, environment: Mapping[str, str],
                  definitions: Sequence[str] = ()) -> Path:
    """Configure and build the title's player product in ``build/player``; returns that directory."""
    build = root / PLAYER_BUILD
    say(f"building {target}…")
    configure_consumer(root, build, "Release", definitions, environment, quiet=True)
    build_targets(build, [target], environment)
    return build


def build_discdump(root: Path, environment: Mapping[str, str]) -> Path:
    """psxport's disc reader, built from the framework tree into the title's ``build/player-tools``."""
    build = root / FRAMEWORK_TOOLS_BUILD
    say("building discdump…")
    configure(FRAMEWORK, build, "Release", [*lightrec_cmake_definitions(environment),
                                            *lightning_cmake_definitions(environment)],
              environment, owner=root, quiet=True)
    build_targets(build, ["discdump"], environment)
    for name in ("discdump", "discdump.exe"):
        candidate = build / "tools" / name
        if os.access(candidate, os.X_OK):
            return candidate
    raise ToolError(f"the discdump build produced no executable under {build / 'tools'}")


def exec_product(program: Path, arguments: Sequence[str], environment: Mapping[str, str],
                 cwd: Path) -> NoReturn:
    """Replace this process with the product."""
    if not os.access(program, os.X_OK):
        raise ToolError(f"the build produced no executable at {program}")
    say(f"launching {program.name}…")
    os.chdir(cwd)
    os.execve(program, [str(program), *arguments], dict(environment))


def argument_parser(description: str, *, disc: bool = True) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    if disc:
        parser.add_argument("disc", nargs="?", help="the disc image; otherwise environment, .env or drop-in")
    parser.add_argument("--prepare-only", action="store_true", help="provision and build without launching")
    return parser


def main(launch: Callable[[], int | None]) -> int:
    """Run a title's launch sequence, turning a refusal into one error line and exit status 2."""
    try:
        return launch() or 0
    except (ToolError, OSError) as error:
        print(f"[run] error: {error}", file=sys.stderr)
        return 2
