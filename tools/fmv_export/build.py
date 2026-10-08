#!/usr/bin/env python3
"""Build the standalone FMV exporter and decoder test with shared runtime sources."""

from __future__ import annotations

import os
import shlex
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from automation.process import ToolError, first_file, require_program, run


def compile_source(compiler: str, root: Path, source: Path, output: Path, flags: list[str]) -> None:
    run([compiler, *flags, "-c", source, "-o", output], cwd=root)


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    runtime = root / "runtime/psx"
    mednafen = root / "vendor/beetle-psx/mednafen"
    output = root / "build/tools/fmv_export"
    output.mkdir(parents=True, exist_ok=True)
    include_flags = [
        f"-I{runtime}",
        f"-I{root / 'vendor/beetle-psx/deps/libchdr/include'}",
        f"-I{root / 'vendor/lucent/include'}",
        f"-I{mednafen}",
        f"-I{mednafen / 'psx'}",
        f"-I{root / 'vendor/beetle-psx'}",
        f"-I{root / 'vendor/beetle-psx/libretro-common/include'}",
    ]
    c_flags = ["-O2", *include_flags]
    cxx_flags = ["-std=c++20", "-O2", *include_flags]

    build_root = root / "build"
    archive_names = ("libchdr-static.a", "libchdr-lzma.a", "libminiz.a")
    archives: list[Path] = []
    for name in archive_names:
        archive = first_file(build_root, name)
        if archive is None:
            print(f"fmv_export build: REFUSED: missing {name}; configure libchdr first", file=sys.stderr)
            return 1
        archives.append(archive)

    try:
        cc = require_program(os.environ.get("CC", "cc"))
        cxx = require_program(os.environ.get("CXX", "c++"))
        zstd = first_file(build_root, "libzstd.a")
        zstd_flags: list[str]
        if zstd is not None:
            zstd_flags = [str(zstd)]
        else:
            pkg_config = require_program("pkg-config")
            zstd_flags = shlex.split(run([pkg_config, "--libs", "libzstd"], cwd=root, capture=True).stdout)

        c_sources = {
            "mdec.o": mednafen / "psx/mdec.c",
            "mdec_beetle.o": runtime / "mdec_beetle.c",
        }
        cxx_sources = {
            "fmv_decode.o": runtime / "fmv_decode.cpp",
            "cfg.o": runtime / "cfg.cpp",
            "lucent_config.o": root / "vendor/lucent/src/config.cpp",
            "lucent_log.o": root / "vendor/lucent/src/log.cpp",
            "fmv_export.o": root / "tools/fmv_export/fmv_export.cpp",
            "test_fmv_decode.o": root / "tools/fmv_export/test_fmv_decode.cpp",
        }
        for name, source in c_sources.items():
            compile_source(cc, root, source, output / name, c_flags)
        for name, source in cxx_sources.items():
            compile_source(cxx, root, source, output / name, cxx_flags)

        common_objects = [
            output / "fmv_decode.o",
            output / "cfg.o",
            output / "lucent_config.o",
            output / "lucent_log.o",
            output / "mdec_beetle.o",
            output / "mdec.o",
        ]
        common_libraries = [*archives, *zstd_flags, "-lpthread", "-lz", "-lm"]
        run(
            [cxx, "-o", output / "fmv_export", output / "fmv_export.o", *common_objects, *common_libraries],
            cwd=root,
        )
        run(
            [
                cxx,
                "-o",
                output / "test_fmv_decode",
                output / "test_fmv_decode.o",
                *common_objects,
                *common_libraries,
                "-lcrypto",
            ],
            cwd=root,
        )
    except ToolError as error:
        print(f"fmv_export build: REFUSED: {error}", file=sys.stderr)
        return 1
    print(f"built {output / 'fmv_export'}")
    print(f"built {output / 'test_fmv_decode'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
