#!/usr/bin/env python3
"""Run the complete asset-free psxport gate."""

from __future__ import annotations

import argparse
from pathlib import Path

from project import DEFAULT_BUILD, build_project, configure, test_project


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()
    build = args.build.resolve()
    configure(build)
    build_project(build)
    test_project(build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
