#!/usr/bin/env python3
"""Configure and build a title repository against this psxport checkout with Ninja.

    CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py \\
        --build build/agent [--target NAME]... [-DNAME=VALUE]...

Run from the title root. The compiler comes from CC and CXX. Lightrec and Lightning locations come
from the same environment variables psxport's own build reads.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError, run  # noqa: E402
from project import lightning_cmake_definitions, lightrec_cmake_definitions  # noqa: E402


def compiler_cmake_definitions(environment: Mapping[str, str]) -> list[str]:
    """Pass CC/CXX explicitly so a requested compiler replaces the one cached in an existing build."""
    variables = (("CC", "CMAKE_C_COMPILER"), ("CXX", "CMAKE_CXX_COMPILER"))
    return [f"-D{cmake}={environment[env]}" for env, cmake in variables if environment.get(env)]


def build_consumer(
    root: Path,
    build: Path,
    psxport: Path,
    definitions: Sequence[str],
    targets: Sequence[str],
    environment: Mapping[str, str],
) -> None:
    root = root.resolve()
    build = build.resolve()
    if not build.is_relative_to(root / "build"):
        raise ToolError(f"build must stay under {root / 'build'}: {build}")
    if not (psxport / "cmake" / "psxport.cmake").is_file():
        raise ToolError(f"no psxport checkout at {psxport}")
    if any(not definition.startswith("-D") for definition in definitions):
        raise ToolError("CMake definitions must use explicit -DNAME=VALUE spelling")
    process_environment = dict(environment)
    process_environment["PSXPORT_DIR"] = str(psxport)
    run(
        [
            "cmake", "-S", root, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            f"-DPython3_EXECUTABLE={sys.executable}",
            f"-DPSXPORT_DIR={psxport}",
            *compiler_cmake_definitions(process_environment),
            *lightrec_cmake_definitions(process_environment),
            *lightning_cmake_definitions(process_environment),
            *definitions,
        ],
        cwd=root,
        environment=process_environment,
    )
    build_command = ["cmake", "--build", build]
    for target in targets:
        build_command += ["--target", target]
    run(build_command, cwd=root, environment=process_environment)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--build", type=Path, default=Path("build/agent"))
    parser.add_argument("--psxport", type=Path, default=None)
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args(argv)
    root = args.source.resolve()
    psxport = (args.psxport or root / "external" / "psxport").resolve()
    build = args.build if args.build.is_absolute() else root / args.build
    try:
        build_consumer(
            root, build, psxport, [f"-D{item}" for item in args.definitions], args.target, os.environ
        )
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
