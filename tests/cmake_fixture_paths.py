"""Explicit dependency inputs shared by nested CMake fixture tests."""

from __future__ import annotations

import os
import shutil
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path


def configured_lightrec() -> Path:
    raw = os.environ.get("PSXPORT_LIGHTREC_DIR")
    if not raw:
        raise RuntimeError(
            "PSXPORT_LIGHTREC_DIR is missing; CTest must pass the exact Lightrec path "
            "resolved by the parent CMake configure"
        )
    checkout = Path(raw).resolve()
    if not (checkout / "lightrec.h").is_file():
        raise RuntimeError(f"configured Lightrec checkout is missing lightrec.h: {checkout}")
    return checkout


def configured_cmake_arguments() -> list[str]:
    """Forward the parent's resolved toolchain and dependency, independent of machine defaults."""
    arguments = ["-G", "Ninja", f"-DPython3_EXECUTABLE={sys.executable}"]
    for definition, variable in (
        ("LIBLIGHTNING", "PSXPORT_FIXTURE_LIGHTNING_LIBRARY"),
        ("LIBLIGHTNING_INCLUDE_DIR", "PSXPORT_FIXTURE_LIGHTNING_INCLUDE"),
        ("CMAKE_C_COMPILER", "PSXPORT_FIXTURE_C_COMPILER"),
        ("CMAKE_CXX_COMPILER", "PSXPORT_FIXTURE_CXX_COMPILER"),
    ):
        value = os.environ.get(variable)
        if not value:
            raise RuntimeError(f"{variable} is missing; run this fixture through configured CTest")
        arguments.append(f"-D{definition}={value}")
    return arguments


@contextmanager
def fixture_workspace(name: str) -> Iterator[Path]:
    """Own one stable build directory; refuse a concurrent or stale fixture instead of erasing it."""
    parent = Path(__file__).resolve().parents[1] / "build" / "test-fixtures"
    parent.mkdir(parents=True, exist_ok=True)
    work = parent / name
    work.mkdir()
    try:
        yield work
    finally:
        shutil.rmtree(work)
