"""Build a title's release AppImage inside the Ubuntu 22.04 build container.

    uv run --frozen python external/psxport/tools/port/appimage_container.py --product <product>

Run from the title root. The image is ``appimage-build.Containerfile`` beside this file, tagged by its
hash and the pinned library sources it bakes in. The title and the framework checkout are
mounted at their host paths; the build, its uv environment and the dependency prefixes stay under the
title's ``build/``, apart from the host's own trees. The package is audited against glibc 2.35 and carries
GitHub-release update information for the title's ``origin``. Locally this runs under podman; CI passes
``--engine docker``.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port.launcher import APPIMAGE_BUILD

FRAMEWORK = Path(__file__).resolve().parents[2]
CONTAINERFILE = Path(__file__).resolve().with_name("appimage-build.Containerfile")
# Files the image COPYs in; their bytes are part of the image tag.
# The image builds the shared pinned libraries; its context is their directory.
LIBRARIES = FRAMEWORK / "vendor" / "target-platform" / "target_platform"
IMAGE_INPUTS = (CONTAINERFILE, LIBRARIES / "libraries.py", LIBRARIES / "libraries.json")
IMAGE = "localhost/psxport-appimage-build"
GLIBC_FLOOR = "2.35"
GITHUB_REMOTE = re.compile(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?/?$")


class Refused(RuntimeError):
    """The package could not be built."""


def run(command: Sequence[str]) -> None:
    print(f"[appimage-container] {' '.join(command)}", flush=True)
    if subprocess.run(list(command), check=False).returncode != 0:
        raise Refused(f"{command[0]} failed")


def image_tag() -> str:
    digest = hashlib.sha256()
    for path in IMAGE_INPUTS:
        digest.update(path.read_bytes())
    return f"{IMAGE}:{digest.hexdigest()[:12]}"


def build_image(engine: str, tag: str) -> None:
    if subprocess.run([engine, "image", "inspect", tag], capture_output=True, check=False).returncode == 0:
        return
    run([engine, "build", "-t", tag, "-f", str(CONTAINERFILE), str(LIBRARIES)])


def engine_options(engine: str) -> list[str]:
    if engine == "podman":
        # Host ownership is kept and SELinux labels left alone instead of relabelling the checkouts.
        return ["--userns=keep-id", "--security-opt", "label=disable"]
    return ["--user", f"{os.getuid()}:{os.getgid()}"]


def origin_repository(root: Path) -> str:
    completed = subprocess.run(["git", "remote", "get-url", "origin"], cwd=root, text=True,
                               capture_output=True, check=False)
    found = GITHUB_REMOTE.search(completed.stdout.strip()) if completed.returncode == 0 else None
    if found is None:
        raise Refused("the title has no GitHub origin to publish updates from; pass --update-repository")
    return found.group(1)


def in_container(engine: str, tag: str, root: Path, command: Sequence[str]) -> None:
    home = root / "build" / "appimage-home"
    home.mkdir(parents=True, exist_ok=True)
    mounts = [root] + ([FRAMEWORK] if not FRAMEWORK.is_relative_to(root) else [])
    run([engine, "run", "--rm", *engine_options(engine),
         *(argument for mount in mounts for argument in ("-v", f"{mount}:{mount}")),
         "-w", str(root),
         "-e", f"HOME={home}",
         "-e", f"UV_PROJECT_ENVIRONMENT={root / 'build' / 'appimage-venv'}",
         "-e", f"PSXPORT_DEPENDENCY_ROOT={root / 'build' / 'appimage-deps'}",
         tag, *command])


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--product", required=True, help="the title's port target, e.g. tomba2_port")
    parser.add_argument("--engine", choices=("podman", "docker"), default="podman")
    parser.add_argument("--version", help="release version; defaults to git describe")
    parser.add_argument("--update-repository", metavar="OWNER/REPO", help="defaults to the title's origin")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="developer staging only; a release is built from a clean commit")
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        if shutil.which(args.engine) is None:
            raise Refused(f"{args.engine} is required to build the release AppImage")
        if not re.fullmatch(r"[A-Za-z0-9_]+", args.product):
            raise Refused(f"--product {args.product!r} is not a target name")
        tag = image_tag()
        build_image(args.engine, tag)
        build = root / APPIMAGE_BUILD
        consumer = FRAMEWORK / "tools" / "port" / "consumer_build.py"
        packager = FRAMEWORK / "tools" / "port" / "appimage.py"
        in_container(args.engine, tag, root, ["uv", "run", "--frozen", "python", str(consumer),
                                              "--build", str(build), "--target", args.product])
        package = ["uv", "run", "--frozen", "python", str(packager), "--binary", str(build / "bin" / args.product),
                   "--max-glibc", GLIBC_FLOOR, "--output", str(root / "build" / "release"),
                   "--update-repository", args.update_repository or origin_repository(root)]
        if args.version:
            package += ["--version", args.version]
        if args.allow_dirty:
            package.append("--allow-dirty")
        in_container(args.engine, tag, root, package)
        return 0
    except Refused as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
