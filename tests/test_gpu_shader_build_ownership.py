#!/usr/bin/env python3
"""CMake integration gate for per-consumer generated shader ownership."""

from __future__ import annotations

import concurrent.futures
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run_result(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    return result


def run(command: list[str], *, cwd: Path) -> None:
    result = run_result(command, cwd=cwd)
    if result.returncode != 0:
        detail = "\n".join(part for part in (result.stdout, result.stderr) if part)
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}")


def configure(source: Path, build: Path) -> None:
    run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_CXX_COMPILER=clang++",
        ],
        cwd=source,
    )


def build_target(source: Path, build: Path, target: str = "shader_consumer") -> None:
    run(["cmake", "--build", str(build), "--target", target, "--verbose"], cwd=source)


def write_positive_fixture(repo: Path, fixture: Path) -> Path:
    fixture_root = fixture / "framework"
    shader_dir = fixture_root / "runtime/psx/shaders_gpu"
    shader_dir.parent.mkdir(parents=True)
    shutil.copytree(repo / "runtime/psx/shaders_gpu", shader_dir)
    tool_dir = fixture_root / "tools"
    tool_dir.mkdir(parents=True)
    shutil.copy2(repo / "tools/gen_gpu_shaders.py", tool_dir / "gen_gpu_shaders.py")
    (fixture_root / "shader_consumer.cpp").write_text(
        '#include "psxport_generated/gpu_vk_shaders.h"\n'
        "unsigned shader_fixture_word() { return spv_g_present_vert[0]; }\n",
        encoding="utf-8",
    )
    (fixture_root / "CMakeLists.txt").write_text(
        "\n".join(
            (
                f'include("{repo / "cmake/gpu_shaders.cmake"}")',
                "add_library(shader_consumer STATIC shader_consumer.cpp)",
                f'psxport_add_gpu_shaders("{fixture_root}" shader_consumer)',
                "",
            )
        ),
        encoding="utf-8",
    )

    source = fixture / "positive"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(shader_output_ownership CXX)",
                f'add_subdirectory("{fixture_root}" psxport_build)',
                "",
            )
        ),
        encoding="utf-8",
    )
    return source


def missing_include_answer(repo: Path, fixture: Path) -> bool:
    module = (repo / "cmake/gpu_shaders.cmake").read_text(encoding="utf-8")
    include_owner = "  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})\n"
    if module.count(include_owner) != 1:
        raise RuntimeError("shipping shader module include-owner anchor changed")

    negative_root = fixture / "missing-include-framework"
    shutil.copytree(fixture / "framework", negative_root)
    mutated_module = negative_root / "gpu_shaders_missing_include.cmake"
    mutated_module.write_text(module.replace(include_owner, ""), encoding="utf-8")
    cmake_file = negative_root / "CMakeLists.txt"
    cmake_file.write_text(
        cmake_file.read_text(encoding="utf-8").replace(
            str(repo / "cmake/gpu_shaders.cmake"), str(mutated_module)
        ),
        encoding="utf-8",
    )

    source = fixture / "missing-include"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(shader_output_missing_include CXX)",
                f'add_subdirectory("{negative_root}" psxport_build)',
                "",
            )
        ),
        encoding="utf-8",
    )
    build = fixture / "missing-include-build"
    configure(source, build)
    result = run_result(
        ["cmake", "--build", str(build), "--target", "shader_consumer"], cwd=source
    )
    output = result.stdout + result.stderr
    return result.returncode != 0 and "psxport_generated/gpu_vk_shaders.h" in output


def write_legacy_fixture(fixture: Path) -> tuple[Path, Path]:
    source = fixture / "legacy"
    source.mkdir()
    shared_header = fixture / "legacy-framework/runtime/psx/gpu_vk_shaders.h"
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
                list(executor.map(lambda build: build_target(positive, build), builds))

            headers = [build / "psxport_build/psxport_generated/gpu_vk_shaders.h" for build in builds]
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
                and not (fixture / "framework/runtime/psx/gpu_vk_shaders.h").exists()
            )
            checks["nested consumer missing include owner fails compilation"] = missing_include_answer(
                repo, fixture
            )

            legacy, shared_header = write_legacy_fixture(fixture)
            legacy_builds = [fixture / "legacy-build-a", fixture / "legacy-build-b"]
            for build in legacy_builds:
                configure(legacy, build)
                build_target(legacy, build, "gen_gpu_shaders")
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
