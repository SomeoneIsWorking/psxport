#!/usr/bin/env python3
"""Configure and build psxport with the canonical maintainer settings."""

from __future__ import annotations

import argparse
from pathlib import Path

from project import DEFAULT_BUILD, build_project, configure


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()
    build = args.build.resolve()
    configure(build)
    build_project(build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
