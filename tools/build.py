#!/usr/bin/env python3
"""Configure and build psxport with the launcher's configure (Ninja, Clang when installed).

`--platform windows|macos` builds against that platform's pinned libraries (`port/release_platform.py`)
in its own build tree. On a Mac, `host` is `macos` built into the host tree, since the Mac's libraries
come from those pins.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from automation.process import ToolError
from port import release_platform
from port.launcher import PLATFORM_BUILDS, build_targets, configure, maintainer_build
from project import DEFAULT_BUILD, ROOT


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--platform", choices=("host", *release_platform.PLATFORMS), default="host")
    args = parser.parse_args()
    environment = dict(os.environ)
    platform_name = "macos" if args.platform == "host" and sys.platform == "darwin" else args.platform
    try:
        if platform_name == "host":
            platform = []
        else:
            # The mipsel test program and psxtest_port are host checks; a platform build is the framework only.
            platform = [*release_platform.definitions(platform_name, environment), "-DPSXPORT_TESTPROG=OFF"]
        build = maintainer_build(ROOT, args.build or PLATFORM_BUILDS.get(args.platform, DEFAULT_BUILD))
        configure(ROOT, build, "RelWithDebInfo", ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *platform], environment)
        build_targets(build, [], environment)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
