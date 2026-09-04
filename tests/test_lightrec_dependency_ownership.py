#!/usr/bin/env python3
"""Exercise Lightrec source identity and consumer-cache ownership in real CMake configures."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from cmake_fixture_paths import configured_lightrec


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "cmake" / "lightrec_dependency.cmake"
LIGHTREC = configured_lightrec()
SCRATCH = ROOT / "scratch" / "lightrec-dependency-ownership"


def run_cmake(source: Path, build: Path, *, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-DCMAKE_C_COMPILER=clang",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if check and completed.returncode != 0:
        raise RuntimeError(
            f"CMake configure failed ({completed.returncode}) for {source}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def write_project(source: Path, body: str, *, lightrec: Path = LIGHTREC) -> None:
    source.mkdir(parents=True, exist_ok=True)
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(lightrec_dependency_fixture LANGUAGES C)",
                f'set(PSXPORT_ROOT "{ROOT}")',
                f'set(PSXPORT_LIGHTREC_DIR "{lightrec}" CACHE PATH "fixture Lightrec")',
                f'include("{MODULE}")',
                body,
                "",
            )
        ),
        encoding="utf-8",
    )


def main() -> int:
    if not (LIGHTREC / "lightrec.h").is_file():
        raise RuntimeError(f"maintained Lightrec checkout is missing: {LIGHTREC}")

    SCRATCH.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=SCRATCH))
    checks = 0
    try:
        clean_lightrec = work / "clean-lightrec"
        subprocess.run(
            ["git", "clone", "--quiet", "--no-local", str(LIGHTREC), str(clean_lightrec)],
            check=True,
        )

        preserved_source = work / "preserved-source"
        write_project(
            preserved_source,
            "\n".join(
                (
                    'set(BUILD_SHARED_LIBS ON CACHE BOOL "consumer shared setting" FORCE)',
                    'set(BUILD_TESTING ON CACHE BOOL "consumer test setting" FORCE)',
                    "set(BUILD_SHARED_LIBS ON)",
                    "set(BUILD_TESTING ON)",
                    "psxport_configure_lightrec_dependency()",
                    "get_target_property(_lightrec_type lightrec TYPE)",
                    'if(NOT _lightrec_type STREQUAL "STATIC_LIBRARY")',
                    '  message(FATAL_ERROR "Lightrec was not scoped to a static dependency")',
                    "endif()",
                    "if(NOT BUILD_SHARED_LIBS OR NOT BUILD_TESTING)",
                    '  message(FATAL_ERROR "consumer normal variables were mutated")',
                    "endif()",
                )
            ),
            lightrec=clean_lightrec,
        )
        preserved_build = work / "preserved-build"
        run_cmake(preserved_source, preserved_build)
        cache = (preserved_build / "CMakeCache.txt").read_text(encoding="utf-8")
        for expected in (
            "BUILD_SHARED_LIBS:BOOL=ON",
            "BUILD_TESTING:BOOL=ON",
            "//consumer shared setting",
            "//consumer test setting",
        ):
            checks += 1
            if expected not in cache:
                raise AssertionError(f"consumer cache state was not restored: {expected}")

        absent_source = work / "absent-source"
        write_project(
            absent_source,
            "\n".join(
                (
                    "psxport_configure_lightrec_dependency()",
                    "if(DEFINED CACHE{BUILD_SHARED_LIBS} OR DEFINED CACHE{BUILD_TESTING})",
                    '  message(FATAL_ERROR "dependency leaked generic cache options")',
                    "endif()",
                )
            ),
            lightrec=clean_lightrec,
        )
        run_cmake(absent_source, work / "absent-build")
        checks += 1

        mismatch_source = work / "mismatch-source"
        (mismatch_source / "fake").mkdir(parents=True)
        (mismatch_source / "fake" / "lightrec.c").write_text("int fake_lightrec;\n", encoding="utf-8")
        write_project(
            mismatch_source,
            "\n".join(
                (
                    "add_library(lightrec STATIC fake/lightrec.c)",
                    "psxport_configure_lightrec_dependency()",
                )
            ),
            lightrec=clean_lightrec,
        )
        mismatch = run_cmake(mismatch_source, work / "mismatch-build", check=False)
        checks += 1
        output = mismatch.stdout + mismatch.stderr
        if mismatch.returncode == 0 or "existing lightrec target source mismatch" not in output:
            raise AssertionError(f"mismatched pre-existing target was not refused:\n{output}")

        dirty_lightrec = work / "dirty-lightrec"
        subprocess.run(
            ["git", "clone", "--quiet", "--no-local", str(clean_lightrec), str(dirty_lightrec)],
            check=True,
        )
        readme = dirty_lightrec / "README.md"
        readme.write_text(readme.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        dirty_source = work / "dirty-source"
        write_project(
            dirty_source,
            "psxport_configure_lightrec_dependency()",
            lightrec=dirty_lightrec,
        )
        dirty = run_cmake(dirty_source, work / "dirty-build", check=False)
        checks += 1
        output = dirty.stdout + dirty.stderr
        if dirty.returncode == 0 or "has worktree changes" not in output:
            raise AssertionError(f"modified pinned source was not refused:\n{output}")

        untracked_lightrec = work / "untracked-lightrec"
        subprocess.run(
            ["git", "clone", "--quiet", "--no-local", str(clean_lightrec), str(untracked_lightrec)],
            check=True,
        )
        (untracked_lightrec / "untracked-source.c").write_text("int injected_source;\n", encoding="utf-8")
        untracked_source = work / "untracked-source"
        write_project(
            untracked_source,
            "psxport_configure_lightrec_dependency()",
            lightrec=untracked_lightrec,
        )
        untracked = run_cmake(untracked_source, work / "untracked-build", check=False)
        checks += 1
        output = untracked.stdout + untracked.stderr
        if untracked.returncode == 0 or "has worktree changes" not in output:
            raise AssertionError(f"untracked pinned source was not refused:\n{output}")
    finally:
        shutil.rmtree(work)

    print(f"lightrec dependency ownership: PASS ({checks} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
