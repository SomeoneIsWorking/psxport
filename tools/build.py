#!/usr/bin/env python3
"""Configure and build psxport with the launcher's configure (Ninja, Clang when installed).

`--platform windows|macos` builds against that platform's pinned libraries (`port/release_platform.py`)
in its own build tree. `host` links the host's own pins (`release_platform.pinned`) in the host tree.
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
    parser.add_argument("--jobs", type=int, help="compile jobs (default: one per usable core)")
    args = parser.parse_args()
    environment = dict(os.environ)
    # The mipsel test program and psxtest_port are Linux host checks; any other build is the framework only.
    testprog = [] if args.platform == "host" and sys.platform == "linux" else ["-DPSXPORT_TESTPROG=OFF"]
    try:
        build = maintainer_build(ROOT, args.build or PLATFORM_BUILDS.get(args.platform, DEFAULT_BUILD))
        configure(ROOT, build, "RelWithDebInfo", ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", *testprog], environment,
                  platform=args.platform)
        build_targets(build, [], environment, args.jobs)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
