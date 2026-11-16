#!/usr/bin/env python3
"""Configure and build a title repository against this psxport checkout with Ninja.

    CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py \\
        [--build build/agent|build/ci] [--platform host|windows|macos] [--target NAME]... [-DNAME=VALUE]...

Run from the title root. The configure is the launcher's (`launcher.configure_consumer`), so the
compiler, Lightrec and Lightning choices are the player's, at RelWithDebInfo. `--platform windows`
cross-compiles with llvm-mingw into `build/windows`; `--platform macos` builds the Intel app on any Mac into
`build/macos`, both against the pinned target libraries (`port/target_platform.py`).
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError
from port import app_icon, desktop_shortcut, target_platform
from port.launcher import (
    AGENT_BUILD,
    PLATFORM_BUILDS,
    build_targets,
    configure_consumer,
    maintainer_build,
)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, help=f"default {AGENT_BUILD}, or the platform's own build tree")
    parser.add_argument("--platform", choices=("host", *target_platform.PLATFORMS), default="host")
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        environment = dict(os.environ)
        definitions = [f"-D{item}" for item in args.definitions]
        if args.platform != "host":
            # The host's Lightning prefix is not the target's.
            environment.pop("PSXPORT_LIGHTNING_PREFIX", None)
            definitions = [*target_platform.provision(args.platform, environment), *definitions]
        build = maintainer_build(root, args.build or PLATFORM_BUILDS.get(args.platform, AGENT_BUILD))
        if args.platform == "windows":
            icon = app_icon.ico(desktop_shortcut.resolve(root).icon, build / "icon" / "app.ico", build / "icon")
            definitions.append(f"-DPSXPORT_PRODUCT_ICON={icon}")
        configure_consumer(root, build, "RelWithDebInfo", definitions, environment)
        build_targets(build, args.target, environment)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
