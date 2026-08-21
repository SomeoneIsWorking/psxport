#!/usr/bin/env python3
"""CMake integration gate for per-consumer generated shader ownership."""

from __future__ import annotations

import concurrent.futures
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], *, cwd: Path) -> None:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = "\n".join(part for part in (result.stdout, result.stderr) if part)
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}")


def configure(source: Path, build: Path) -> None:
    run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"], cwd=source)


def build_shader(source: Path, build: Path) -> None:
    run(["cmake", "--build", str(build), "--target", "gen_gpu_shaders"], cwd=source)


def write_positive_fixture(repo: Path, fixture: Path) -> Path:
    fixture_root = fixture / "framework"
    shader_dir = fixture_root / "runtime/recomp/shaders_gpu"
    shader_dir.parent.mkdir(parents=True)
    shutil.copytree(repo / "runtime/recomp/shaders_gpu", shader_dir)
    tool_dir = fixture_root / "tools"
    tool_dir.mkdir(parents=True)
    shutil.copy2(repo / "tools/gen_gpu_shaders.py", tool_dir / "gen_gpu_shaders.py")

    source = fixture / "positive"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(shader_output_ownership NONE)",
                f'include("{repo / "cmake/gpu_shaders.cmake"}")',
                f'psxport_add_gpu_shaders("{fixture_root}" SHADER_HEADER)',
                "",
            )
        ),
        encoding="utf-8",
    )
    return source


def write_legacy_fixture(fixture: Path) -> tuple[Path, Path]:
    source = fixture / "legacy"
    source.mkdir()
    shared_header = fixture / "legacy-framework/runtime/recomp/gpu_vk_shaders.h"
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(shared_shader_output NONE)",
                f'set(SHARED_HEADER "{shared_header}")',
                "get_filename_component(SHARED_DIR ${SHARED_HEADER} DIRECTORY)",
                "set(STAMP ${CMAKE_CURRENT_BINARY_DIR}/shader.stamp)",
                "add_custom_command(OUTPUT ${STAMP}",
                "  BYPRODUCTS ${SHARED_HEADER}",
                "  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHARED_DIR}",
                "  COMMAND ${CMAKE_COMMAND} -E touch ${SHARED_HEADER}",
                "  COMMAND ${CMAKE_COMMAND} -E touch ${STAMP})",
                "add_custom_target(gen_gpu_shaders",
                "  COMMAND ${CMAKE_COMMAND} -E touch ${SHARED_HEADER}",
                "  DEPENDS ${STAMP})",
                "",
            )
        ),
        encoding="utf-8",
    )
    return source, shared_header


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    scratch = repo / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    checks: dict[str, bool] = {}

    try:
        with tempfile.TemporaryDirectory(prefix="shader-owner-", dir=scratch) as temp_name:
            fixture = Path(temp_name)
            positive = write_positive_fixture(repo, fixture)
            builds = [fixture / "positive-build-a", fixture / "positive-build-b"]
            for build in builds:
                configure(positive, build)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                list(executor.map(lambda build: build_shader(positive, build), builds))

            headers = [build / "psxport_generated/gpu_vk_shaders.h" for build in builds]
            checks["concurrent builds own byte-identical headers"] = (
                headers[0].is_file()
                and headers[1].is_file()
                and headers[0].read_bytes() == headers[1].read_bytes()
            )
            peer_bytes = headers[1].read_bytes()
            run(["cmake", "--build", str(builds[0]), "--target", "clean"], cwd=positive)
            checks["clean removes only its build-owned header"] = (
                not headers[0].exists()
                and headers[1].read_bytes() == peer_bytes
                and not (fixture / "framework/runtime/recomp/gpu_vk_shaders.h").exists()
            )

            legacy, shared_header = write_legacy_fixture(fixture)
            legacy_builds = [fixture / "legacy-build-a", fixture / "legacy-build-b"]
            for build in legacy_builds:
                configure(legacy, build)
                build_shader(legacy, build)
            run(["cmake", "--build", str(legacy_builds[0]), "--target", "clean"], cwd=legacy)
            checks["legacy shared byproduct reproduces peer deletion"] = not shared_header.exists()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"[shader-build-ownership] ERROR {error}", file=sys.stderr)
        return 1

    for name, passed in checks.items():
        print(f"[shader-build-ownership] {'PASS' if passed else 'FAIL'} {name}")
    passed = sum(checks.values())
    print(f"[shader-build-ownership] {passed}/{len(checks)} checks passed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
