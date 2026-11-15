#!/usr/bin/env python3
"""Configure and build psxport with the launcher's configure (Ninja, Clang when installed).

`--platform windows|macos` builds against that platform's pinned libraries and Lightning
(`port/target_platform.py`) in its own build tree.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from automation.process import ToolError
from port import target_platform
from port.launcher import PLATFORM_BUILDS, build_targets, configure, maintainer_build
from project import (
    DEFAULT_BUILD,
    ROOT,
    lightning_cmake_definitions,
    lightrec_cmake_definitions,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--platform", choices=("host", *target_platform.PLATFORMS), default="host")
    args = parser.parse_args()
    environment = dict(os.environ)
    try:
        if args.platform == "host":
            platform = lightning_cmake_definitions(environment)
        else:
            # The host's Lightning prefix is not the target's.
            environment.pop("PSXPORT_LIGHTNING_PREFIX", None)
            platform = target_platform.provision(args.platform, environment)
        build = maintainer_build(ROOT, args.build or PLATFORM_BUILDS.get(args.platform, DEFAULT_BUILD))
        configure(ROOT, build, "RelWithDebInfo",
                  ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *lightrec_cmake_definitions(environment), *platform],
                  environment)
        build_targets(build, [], environment)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
