"""One Linux CI dependency policy for psxport and its asset-free consumers."""

from __future__ import annotations

import argparse
import logging
import os
import subprocess
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

PACKAGES = (
    "autoconf", "automake", "clang", "clang-format", "clang-tidy", "cmake", "glslc",
    "libfreetype-dev", "libtool", "make", "libsdl3-dev", "libsdl3-image-dev",
    "ninja-build", "pkg-config", "zlib1g-dev",
)
FRAMEWORK_SUBMODULES = ("external/psycross", "vendor/beetle-psx", "vendor/lucent")
Runner = Callable[[Sequence[str], Path], None]


@dataclass(frozen=True)
class SetupPaths:
    workspace: Path
    framework: Path
    dependencies: Path
    environment_file: Path

    @classmethod
    def from_environment(cls, environment: Mapping[str, str]) -> SetupPaths:
        workspace = Path(environment["GITHUB_WORKSPACE"]).resolve()
        framework = (workspace / environment["PSXPORT_CI_FRAMEWORK"]).resolve()
        dependencies = (workspace / environment["PSXPORT_CI_DEPENDENCIES"]).resolve()
        if not framework.is_relative_to(workspace):
            raise RuntimeError("framework checkout must stay inside the CI workspace")
        if not dependencies.is_relative_to(workspace / "build"):
            raise RuntimeError("CI dependencies must stay under the workspace build directory")
        if dependencies == workspace / "build":
            raise RuntimeError("CI dependencies require a named child of the workspace build directory")
        return cls(workspace, framework, dependencies, Path(environment["GITHUB_ENV"]))


def run(command: Sequence[str], cwd: Path) -> None:
    logging.info("%s", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def provision(paths: SetupPaths, runner: Runner = run) -> dict[str, str]:
    lightrec = paths.dependencies / "lightrec"
    lightning = paths.dependencies / "lightning"
    for required in (
        paths.framework / "cmake/psxport.cmake",
        lightrec / "lightrec.h",
        lightning / "tools/build_lightning.py",
    ):
        if not required.is_file():
            raise RuntimeError(f"required pinned checkout input is missing: {required}")
    runner(["git", "submodule", "update", "--init", *FRAMEWORK_SUBMODULES], paths.framework)
    runner(["sudo", "apt-get", "update"], paths.workspace)
    runner(["sudo", "apt-get", "install", "--yes", *PACKAGES], paths.workspace)
    runner(
        ["uv", "run", "--frozen", "python", "tools/build_lightning.py", "--clean", "--cc", "clang",
         "--skip-upstream-tests"],
        lightning,
    )
    prefix = lightning / "build/install/linux-x86_64"
    for required in (prefix / "include/lightning.h", prefix / "lib/liblightning.a"):
        if not required.is_file():
            raise RuntimeError(f"GNU Lightning build did not produce its required input: {required}")
    return {
        "PSXPORT_LIGHTREC_DIR": str(lightrec),
        "PSXPORT_LIGHTNING_PREFIX": str(prefix),
    }


def export_environment(path: Path, values: Mapping[str, str]) -> None:
    if any("\n" in value or "\r" in value for value in values.values()):
        raise RuntimeError("CI environment paths cannot contain line breaks")
    with path.open("a", encoding="utf-8") as output:
        output.writelines(f"{key}={value}\n" for key, value in values.items())


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="[setup-linux] %(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    try:
        paths = SetupPaths.from_environment(os.environ)
        if not args.validate_only:
            export_environment(paths.environment_file, provision(paths))
    except (KeyError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        logging.error("REFUSED: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
