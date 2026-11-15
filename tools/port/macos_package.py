"""Build a title's macOS x86-64 app without game files.

    uv run --frozen python external/psxport/tools/port/macos_package.py --binary build/macos/bin/<product>

Run on a Mac from the title root after `consumer_build.py --platform macos`. The archive holds `<Name>.app`:
the port as `Contents/MacOS/<Name>`, the framework's assets and the title's licence in `Contents/Resources`
with an icon drawn from the title's SVG, and the pinned MoltenVK in `Contents/Frameworks`, which SDL's
Vulkan backend runs on (`RenderDevice::useBundledMoltenVK`). The port treats that layout as its package
(`psx::config::Config`). The app is signed ad hoc; it runs natively on Intel and under Rosetta on Apple
silicon.

Exit 0: the archive was built and audited. 1: an input contradicted the release contract. 2: the build
could not be performed.
"""

from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port import target_platform
from port.release_package import Mismatch, Refused, Title, audit_content, release_version, resolve_title, run_checked, sha256

FRAMEWORK = Path(__file__).resolve().parents[2]
ASSETS = FRAMEWORK / "assets"
ICON_SIZES = (16, 32, 128, 256, 512)
# Libraries macOS itself provides; everything else must be inside the bundle.
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")


def icon(svg: Path, work: Path, destination: Path) -> None:
    """An .icns from the title's SVG: rsvg-convert renders each size, iconutil packs them."""
    iconset = work / "icon.iconset"
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for size in ICON_SIZES:
        for scale in (1, 2):
            suffix = "" if scale == 1 else "@2x"
            run_checked(["rsvg-convert", "-w", str(size * scale), "-h", str(size * scale), "-o",
                         str(iconset / f"icon_{size}x{size}{suffix}.png"), str(svg)])
    run_checked(["iconutil", "-c", "icns", "-o", str(destination), str(iconset)])


def info_plist(title: Title, version: str) -> bytes:
    return plistlib.dumps({
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": title.name.replace("_", " "),
        "CFBundleExecutable": title.name,
        "CFBundleIconFile": "icon.icns",
        "CFBundleIdentifier": title.entry.app_id,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": title.name,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version.lstrip("v"),
        "CFBundleVersion": version.lstrip("v"),
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": target_platform.MACOS_DEPLOYMENT_TARGET,
        "NSHighResolutionCapable": True,
    })


def stage(title: Title, binary: Path, work: Path, version: str) -> Path:
    app = work / f"{title.name}.app"
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Resources").mkdir()
    (contents / "Frameworks").mkdir()
    executable = contents / "MacOS" / title.name
    shutil.copy2(binary, executable)
    run_checked(["strip", "-x", str(executable)])
    (contents / "Info.plist").write_bytes(info_plist(title, version))
    if not ASSETS.is_dir():
        raise Refused(f"framework assets are missing at {ASSETS}")
    shutil.copytree(ASSETS, contents / "Resources" / "assets")
    shutil.copy2(title.licence, contents / "Resources" / title.licence.name)
    icon(title.entry.icon, work, contents / "Resources" / "icon.icns")
    moltenvk = target_platform.moltenvk(target_platform.dependency_root(os.environ))
    shutil.copy2(moltenvk, contents / "Frameworks" / moltenvk.name)
    # MoltenVK is Apache-2.0; its licence travels with it.
    shutil.copy2(moltenvk.parents[4] / "LICENSE", contents / "Resources" / "MoltenVK-LICENSE")
    return app


def audit_links(app: Path) -> int:
    """Every Mach-O in the bundle links only macOS's own libraries."""
    checked = 0
    for path in (app / "Contents" / "MacOS").iterdir():
        completed = subprocess.run(["otool", "-L", str(path)], text=True, capture_output=True, check=False)
        if completed.returncode != 0:
            raise Refused(f"otool cannot read {path}: {completed.stderr.strip()}")
        for line in completed.stdout.splitlines()[1:]:
            library = line.strip().split(" (", 1)[0]
            if library and not library.startswith(SYSTEM_PREFIXES):
                raise Mismatch(f"{path.name} links {library}, which macOS does not ship")
        checked += 1
    return checked


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="the built port")
    parser.add_argument("--version", help="release version; defaults to git describe")
    parser.add_argument("--output", type=Path, help="output directory; defaults to build/package/macos/out")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="developer staging only; a release is built from a clean commit")
    args = parser.parse_args(argv)
    root = Path.cwd()
    work = root / "build" / "package" / "macos"
    try:
        if sys.platform != "darwin":
            raise Refused("the macOS app is built on macOS (iconutil, otool, codesign)")
        title = resolve_title(root)
        binary = args.binary.resolve()
        if not binary.is_file():
            raise Refused(f"the port is missing at {binary}")
        version, _epoch = release_version(root, args.version, args.allow_dirty)
        app = stage(title, binary, work, version)
        print(f"[macos] link audit: {audit_links(app)} executable(s), macOS libraries only")
        print(f"[macos] content audit: {audit_content(title, app, 'app')} packaged files, 0 game files")
        run_checked(["codesign", "--force", "--deep", "--sign", "-", str(app)])
        run_checked(["codesign", "--verify", "--deep", "--strict", str(app)])
        output = args.output or work / "out"
        output.mkdir(parents=True, exist_ok=True)
        destination = output / f"{title.name}-{version}-macos-x86_64.zip"
        destination.unlink(missing_ok=True)
        run_checked(["ditto", "-c", "-k", "--keepParent", str(app), str(destination)])
        print(f"[macos] artifact: {destination} ({destination.stat().st_size} bytes, sha256 {sha256(destination)})")
        return 0
    except Mismatch as error:
        print(f"MISMATCH: {error}", file=sys.stderr)
        return 1
    except (Refused, target_platform.ToolError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
