#!/usr/bin/env python3
"""Canonical CMake operations for asset-free psxport builds and verification."""

from __future__ import annotations

import os
import sys
from collections.abc import Mapping
from pathlib import Path

from automation.process import ToolError, run


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build" / "ci"


def lightning_cmake_definitions(environment: Mapping[str, str]) -> list[str]:
    """Resolve an explicitly selected maintained-Lightning install prefix for Lightrec."""

    configured_prefix = environment.get("PSXPORT_LIGHTNING_PREFIX")
    if not configured_prefix:
        return []

    prefix = Path(configured_prefix).resolve()
    include_directory = prefix / "include"
    header = include_directory / "lightning.h"
    library = prefix / "lib" / "liblightning.a"
    missing = [path for path in (header, library) if not path.is_file()]
    if missing:
        missing_paths = ", ".join(str(path) for path in missing)
        raise ToolError(
            f"PSXPORT_LIGHTNING_PREFIX is incomplete at {prefix}; missing: {missing_paths}"
        )
    return [
        f"-DLIBLIGHTNING_INCLUDE_DIR={include_directory}",
        f"-DLIBLIGHTNING={library}",
    ]


def lightrec_cmake_definitions(environment: Mapping[str, str]) -> list[str]:
    """Make an explicitly selected Lightrec checkout override any stale CMake cache path."""

    configured_checkout = environment.get("PSXPORT_LIGHTREC_DIR")
    if not configured_checkout:
        return []

    checkout = Path(configured_checkout).resolve()
    missing = [path for path in (checkout / "CMakeLists.txt", checkout / "lightrec.h") if not path.is_file()]
    if missing:
        missing_paths = ", ".join(str(path) for path in missing)
        raise ToolError(f"PSXPORT_LIGHTREC_DIR is incomplete at {checkout}; missing: {missing_paths}")
    return [f"-DPSXPORT_LIGHTREC_DIR={checkout}"]


def configure(build: Path) -> None:
    environment = os.environ.copy()
    lightning_definitions = lightning_cmake_definitions(environment)
    lightrec_definitions = lightrec_cmake_definitions(environment)
    run(
        [
            "cmake",
            "-S",
            ROOT,
            "-B",
            build,
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DPython3_EXECUTABLE={sys.executable}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DPSXPORT_BUILD_SMOKE=ON",
            "-DPSXPORT_BUILD_TESTS=ON",
            *lightrec_definitions,
            *lightning_definitions,
        ],
        cwd=ROOT,
        environment=environment,
    )


def build_project(build: Path) -> None:
    run(["cmake", "--build", build], cwd=ROOT)


def test_project(build: Path) -> None:
    run(["ctest", "--test-dir", build, "--output-on-failure"], cwd=ROOT)
