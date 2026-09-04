#!/usr/bin/env python3
"""Build the vendored RmlUi Core and Debugger static libraries."""

from __future__ import annotations

import sys
from pathlib import Path

from automation.process import ToolError, cpu_jobs, require_program, run


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    source = root / "vendor/rmlui"
    output = root / "build/rmlui"
    existing = next(output.rglob("librmlui.a"), None) if output.exists() else None
    if existing is not None:
        print(f"[build_rmlui] up to date ({existing.relative_to(root)})")
        return 0
    try:
        cmake = require_program("cmake")
        pkg_config = require_program("pkg-config")
        run([pkg_config, "--exists", "freetype2"], cwd=root)
        run(
            [
                cmake,
                "--fresh",
                "-S",
                source,
                "-B",
                output,
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
                "-DBUILD_SHARED_LIBS=OFF",
                "-DRMLUI_SAMPLES=OFF",
                "-DRMLUI_LUA_BINDINGS=OFF",
                "-DRMLUI_SVG_PLUGIN=OFF",
                "-DRMLUI_LOTTIE_PLUGIN=OFF",
                "-DRMLUI_FONT_ENGINE=freetype",
            ],
            cwd=root,
        )
        run(
            [cmake, "--build", output, "-j", str(cpu_jobs()), "--target", "rmlui_core", "rmlui_debugger"],
            cwd=root,
        )
    except ToolError as error:
        detail = str(error)
        if "freetype2" in detail:
            detail += "; install with `sudo dnf install freetype-devel` on Fedora"
        print(f"[build_rmlui] REFUSED: {detail}", file=sys.stderr)
        return 1
    print("[build_rmlui] built:")
    for library in sorted(output.rglob("librmlui*.a")):
        print(f"  {library.relative_to(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
