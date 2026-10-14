#!/usr/bin/env python3
"""Locate the shared re-harness checkout for psxport and every port that builds against it.

Order: `RE_HARNESS_DIR`, then the nearest `shared/re-harness` above this psxport checkout (the
workspace's `~/repo/shared/re-harness`, or `external/shared/re-harness` beside a cloned framework).
Anything else is refused with the clone command. `--fetch` makes the shallow clone a framework clone
needs in CI; `psxport_fetch.py --auto` calls it.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import subprocess
import sys
from pathlib import Path
from types import ModuleType

PSXPORT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_URL = "https://github.com/SomeoneIsWorking/re-harness.git"
MARKER = Path("tools") / "cpp_policy.py"


class HarnessMissing(RuntimeError):
    pass


def source_url() -> str:
    return os.environ.get("RE_HARNESS_URL") or DEFAULT_URL


def workspace_target() -> Path:
    return PSXPORT_ROOT.parents[1] / "shared" / "re-harness"


def clone_target() -> Path:
    return PSXPORT_ROOT.parent / "shared" / "re-harness"


def locate() -> Path:
    override = os.environ.get("RE_HARNESS_DIR")
    if override:
        path = Path(override).resolve()
        if not (path / MARKER).is_file():
            raise HarnessMissing(f"RE_HARNESS_DIR={override} has no {MARKER}")
        return path
    for parent in PSXPORT_ROOT.parents:
        candidate = parent / "shared" / "re-harness"
        if (candidate / MARKER).is_file():
            return candidate
    raise HarnessMissing(
        f"re-harness not found above {PSXPORT_ROOT}; set RE_HARNESS_DIR or run: "
        f"git clone --depth 1 {source_url()} {workspace_target()}"
    )


def tool(name: str) -> Path:
    path = locate() / "tools" / name
    if not path.is_file():
        raise HarnessMissing(f"re-harness at {path.parents[1]} has no tools/{name}")
    return path


def load(module: str) -> ModuleType:
    path = tool(f"{module}.py")
    spec = importlib.util.spec_from_file_location(f"re_harness_{module}", path)
    if spec is None or spec.loader is None:
        raise HarnessMissing(f"cannot import {path}")
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def require(module: str) -> ModuleType:
    """`load`, or a refusal exit for a script that cannot run without the module."""
    try:
        return load(module)
    except HarnessMissing as error:
        raise SystemExit(f"[re-harness] REFUSED: {error}") from None


def fetch() -> Path:
    try:
        return locate()
    except HarnessMissing:
        pass
    target = clone_target()
    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"[re-harness] cloning {source_url()} into {target}")
    done = subprocess.run(["git", "clone", "--depth", "1", source_url(), str(target)], check=False)
    if done.returncode != 0:
        raise HarnessMissing(f"git clone of {source_url()} into {target} failed")
    return locate()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch", action="store_true", help="shallow-clone re-harness when none is found")
    arguments = parser.parse_args(argv)
    try:
        print(fetch() if arguments.fetch else locate())
    except HarnessMissing as error:
        print(f"[re-harness] REFUSED: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
