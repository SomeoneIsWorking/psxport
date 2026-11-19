"""Build a title's Windows x86-64 archive without game files.

    uv run --frozen python external/psxport/tools/port/windows_package.py --binary build/windows/bin/<product>.exe

Run from the title root after `consumer_build.py --platform windows`. The archive holds one directory,
`<Name>/`, with `<Name>.exe` (stripped), the framework's redistributable assets beside it and the title's
licence; the port treats an executable beside `assets/` as its package (`psx::config::Config`). Every DLL the
executable imports must be part of Windows itself.

Exit 0: the archive was built and audited. 1: an input contradicted the release contract. 2: the build
could not be performed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
import zipfile
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError
from port import release_platform
from target_platform import provision as target_platform
from target_platform import windows_imports
from port.release_package import (
    Mismatch,
    Refused,
    Title,
    audit_content,
    release_version,
    resolve_title,
    run_checked,
    sha256,
)

FRAMEWORK = Path(__file__).resolve().parents[2]
ASSETS = FRAMEWORK / "assets"
def stage(title: Title, binary: Path, tree: Path, strip: Path) -> Path:
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    executable = tree / f"{title.name}.exe"
    run_checked([str(strip), "--strip-all", "-o", str(executable), str(binary)])
    if not ASSETS.is_dir():
        raise Refused(f"framework assets are missing at {ASSETS}")
    shutil.copytree(ASSETS, tree / "assets")
    shutil.copy2(title.licence, tree / f"{title.licence.name}.txt")
    return executable


def archive(tree: Path, destination: Path, epoch: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    # Zip timestamps start in 1980; a fixed one keeps the archive reproducible from the commit.
    stamp = time.gmtime(max(epoch, 315532800))[:6]
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in sorted(tree.rglob("*")):
            if path.is_dir():
                continue
            entry = zipfile.ZipInfo(str(Path(tree.name) / path.relative_to(tree)).replace(os.sep, "/"), stamp)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            output.writestr(entry, path.read_bytes())


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="the built port (.exe)")
    parser.add_argument("--version", help="release version; defaults to git describe")
    parser.add_argument("--output", type=Path, help="output directory; defaults to build/package/windows/out")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="developer staging only; a release is built from a clean commit")
    args = parser.parse_args(argv)
    root = Path.cwd()
    work = root / "build" / "package" / "windows"
    try:
        title = resolve_title(root)
        binary = args.binary.resolve()
        if not binary.is_file():
            raise Refused(f"the port is missing at {binary}")
        version, epoch = release_version(root, args.version, args.allow_dirty)
        toolchain = target_platform.llvm_mingw(release_platform.dependency_root(os.environ))
        executable = stage(title, binary, work / title.name, toolchain / "bin" / "llvm-strip")
        try:
            imports = windows_imports.audit(executable, toolchain)
        except windows_imports.ForeignImport as error:
            raise Mismatch(str(error)) from error
        print(f"[windows] import audit: {len(imports)} DLLs, all shipped with Windows")
        files = audit_content(title, work / title.name, "archive")
        print(f"[windows] content audit: {files} packaged files, 0 game files")
        destination = (args.output or work / "out") / f"{title.name}-{version}-windows-x86_64.zip"
        archive(work / title.name, destination, epoch)
        print(f"[windows] artifact: {destination} ({destination.stat().st_size} bytes, sha256 {sha256(destination)})")
        return 0
    except Mismatch as error:
        print(f"MISMATCH: {error}", file=sys.stderr)
        return 1
    except (Refused, ToolError, target_platform.ToolError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
