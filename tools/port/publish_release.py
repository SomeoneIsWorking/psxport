"""Publish a title's GitHub release from this machine: the AppImage and the Windows archive are built here, the
macOS app by the hosted workflow on an Intel Mac.

    uv run --frozen python external/psxport/tools/port/publish_release.py --product <product> --tag vX.Y.Z

Run from a clean title root whose HEAD is on ``origin``. The tag is pushed at HEAD, the release created with
both local packages, and the title's ``release.yml`` dispatched with ``platforms=macos`` to add the app.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port.appimage_container import Refused, origin_repository

PORT_TOOLS = Path(__file__).resolve().parent
RELEASE_DIR = Path("build") / "release"
NOTES = "Bring your own disc: the first-run screen asks for it."


def run(command: Sequence[str], cwd: Path) -> str:
    print(f"[publish] {' '.join(command)}", flush=True)
    completed = subprocess.run(list(command), cwd=cwd, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise Refused(f"{command[0]} {command[1] if len(command) > 1 else ''} failed:\n"
                      f"{(completed.stdout + completed.stderr).strip()[-4000:]}")
    return completed.stdout


def tool(name: str, *arguments: str) -> list[str]:
    return ["uv", "run", "--frozen", "python", str(PORT_TOOLS / name), *arguments]


def require_published_head(root: Path, tag: str) -> None:
    if run(["git", "status", "--porcelain"], root).strip():
        raise Refused("the title tree is dirty; a release is built from a clean commit")
    run(["git", "fetch", "--quiet", "origin"], root)
    head = run(["git", "rev-parse", "HEAD"], root).strip()
    if run(["git", "branch", "-r", "--contains", head], root).strip() == "":
        raise Refused("HEAD is not on origin; push it first")
    if run(["git", "ls-remote", "--tags", "origin", f"refs/tags/{tag}"], root).strip():
        raise Refused(f"origin already has {tag}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--product", required=True, help="the title's port target, e.g. tomba2_port")
    parser.add_argument("--tag", required=True, help="the release tag, vX.Y.Z")
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        if not re.fullmatch(r"v\d+\.\d+\.\d+", args.tag):
            raise Refused(f"--tag {args.tag!r} is not vX.Y.Z")
        repository = origin_repository(root)
        require_published_head(root, args.tag)
        release = root / RELEASE_DIR
        for stale in release.glob("*"):
            if stale.is_file():
                stale.unlink()
        run(tool("appimage_container.py", "--product", args.product, "--version", args.tag), root)
        run(tool("consumer_build.py", "--platform", "windows", "--target", args.product), root)
        run(tool("windows_package.py", "--binary", f"build/windows/bin/{args.product}.exe", "--version", args.tag,
                 "--output", str(RELEASE_DIR)), root)
        packages = sorted(str(path) for path in release.iterdir()
                          if path.name.endswith((".AppImage", ".AppImage.zsync", "-windows-x86_64.zip")))
        if not any(name.endswith(".AppImage") for name in packages) or not any(name.endswith(".zip") for name in packages):
            raise Refused(f"{release} lacks the AppImage or the Windows archive: {packages}")
        run(["git", "tag", args.tag], root)
        run(["git", "push", "--quiet", "origin", f"refs/tags/{args.tag}"], root)
        run(["gh", "release", "create", args.tag, "--repo", repository, "--verify-tag", "--title", args.tag,
             "--notes", NOTES, *packages], root)
        run(["gh", "workflow", "run", "release.yml", "--repo", repository, "--ref", "main",
             "-f", f"tag={args.tag}", "-f", "platforms=macos"], root)
    except Refused as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2
    print(f"[publish] {repository} {args.tag}: {len(packages)} files here, macOS dispatched", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
