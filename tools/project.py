#!/usr/bin/env python3
"""Canonical CMake operations for asset-free psxport builds and verification."""

from __future__ import annotations

import os
from pathlib import Path

from automation.process import run


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build" / "ci"


def configure(build: Path) -> None:
    environment = os.environ.copy()
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
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DPSXPORT_BUILD_SMOKE=ON",
            "-DPSXPORT_BUILD_TESTS=ON",
        ],
        cwd=ROOT,
        environment=environment,
    )


def build_project(build: Path) -> None:
    run(["cmake", "--build", build], cwd=ROOT)


def test_project(build: Path) -> None:
    run(["ctest", "--test-dir", build, "--output-on-failure"], cwd=ROOT)
