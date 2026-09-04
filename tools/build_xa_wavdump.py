#!/usr/bin/env python3
"""Build the standalone XA extractor against the existing libchdr build."""

from __future__ import annotations

import os
import shlex
import sys
from pathlib import Path

from automation.process import ToolError, first_file, require_program, run


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    build = root / "build"
    chdr = first_file(build, "libchdr-static.a")
    if chdr is None:
        print("build_xa_wavdump: REFUSED: libchdr not built; configure the project first", file=sys.stderr)
        return 1
    archives = [chdr]
    for name in ("libchdr-lzma.a", "libminiz.a"):
        archive = first_file(build, name)
        if archive is None:
            print(f"build_xa_wavdump: REFUSED: required archive missing: {name}", file=sys.stderr)
            return 1
        archives.append(archive)
    zstd = first_file(build, "libzstd.a")
    extra = [str(zstd)] if zstd else []
    try:
        compiler = require_program(os.environ.get("CC", "cc"))
        if zstd is None:
            pkg_config = require_program("pkg-config")
            result = run([pkg_config, "--libs", "libzstd"], cwd=root, capture=True)
            extra = shlex.split(result.stdout)
        run(
            [
                compiler,
                "-O2",
                "-o",
                root / "tools/xa_wavdump",
                root / "tools/xa_wavdump.c",
                f"-I{root / 'vendor/beetle-psx/deps/libchdr/include'}",
                *archives,
                *extra,
                "-lpthread",
            ],
            cwd=root,
        )
    except ToolError as error:
        print(f"build_xa_wavdump: REFUSED: {error}", file=sys.stderr)
        return 1
    print("built tools/xa_wavdump")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
