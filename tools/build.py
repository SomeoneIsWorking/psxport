#!/usr/bin/env python3
"""Configure and build psxport with the launcher's configure (Ninja, Clang when installed)."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from automation.process import ToolError
from port.launcher import build_targets, configure
from project import DEFAULT_BUILD, ROOT, lightning_cmake_definitions, lightrec_cmake_definitions


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()
    environment = os.environ
    try:
        configure(ROOT, args.build.resolve(), "RelWithDebInfo",
                  ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *lightrec_cmake_definitions(environment),
                   *lightning_cmake_definitions(environment)], environment)
        build_targets(args.build.resolve(), [], environment)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
