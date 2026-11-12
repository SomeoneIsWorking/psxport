#!/usr/bin/env python3
"""Configure and build a title repository against this psxport checkout with Ninja.

    CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py \\
        [--build build/agent|build/ci] [--target NAME]... [-DNAME=VALUE]...

Run from the title root. The configure is the launcher's (`launcher.configure_consumer`), so the
compiler, Lightrec and Lightning choices are the player's, at RelWithDebInfo.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError  # noqa: E402
from port.launcher import AGENT_BUILD, build_targets, configure_consumer, maintainer_build  # noqa: E402


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, default=AGENT_BUILD)
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("-D", dest="definitions", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        build = maintainer_build(root, args.build)
        configure_consumer(root, build, "RelWithDebInfo", [f"-D{item}" for item in args.definitions],
                           os.environ)
        build_targets(build, args.target, os.environ)
    except ToolError as error:
        print(f"[build] FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
