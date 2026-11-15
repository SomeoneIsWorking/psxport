"""Build a title's x86_64 AppImage without game files.

    uv run --frozen python external/psxport/tools/port/appimage.py --binary build/ci/bin/<product>

Run from the title root. The title owns ``packaging/linux/<app-id>.desktop``, the SVG its ``Icon=`` names
and optionally ``<app-id>.metainfo.xml``; the package holds the built port, the framework's
redistributable assets and the title's licence. The port's first-run screen asks for the player's disc.
linuxdeploy, appimagetool, the type-2 runtime and patchelf are the checksum-pinned downloads in
``appimage-tools.json``; linuxdeploy's own patchelf 0.15 writes libraries that crash the loader on current
toolchains, so it runs the pinned one.

Exit 0: the AppImage was built and audited. 1: an input contradicted the release contract. 2: the build
could not be performed.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port import desktop_shortcut
from port.release_package import (
    Mismatch,
    Refused,
    Title,
    audit_content,
    read_json,
    release_version,
    resolve_title,
    run_checked,
    sha256,
)

FRAMEWORK = Path(__file__).resolve().parents[2]
TOOLS_MANIFEST = Path(__file__).resolve().with_name("appimage-tools.json")
ASSETS = FRAMEWORK / "assets"
GLIBC_REFERENCE = re.compile(r"\bGLIBC_(\d+)\.(\d+)(?:\.(\d+))?\b")
ELF_MAGIC = b"\x7fELF"


@dataclass(frozen=True)
class Tool:
    name: str
    url: str
    sha256: str


def load_tools() -> dict[str, Tool]:
    raw = read_json(TOOLS_MANIFEST)
    if not isinstance(raw, dict) or raw.get("schema") != 1:
        raise Refused(f"{TOOLS_MANIFEST} must use schema 1")
    architectures = raw.get("architectures")
    machine = platform.machine().lower()
    architecture = {"amd64": "x86_64", "x64": "x86_64"}.get(machine, machine)
    selected = architectures.get(architecture) if isinstance(architectures, dict) else None
    if not isinstance(selected, dict):
        raise Refused(f"no pinned AppImage tools are declared for {architecture}")
    tools: dict[str, Tool] = {}
    for name in ("linuxdeploy", "appimagetool", "runtime", "patchelf"):
        entry = selected.get(name)
        url = entry.get("url") if isinstance(entry, dict) else None
        digest = entry.get("sha256") if isinstance(entry, dict) else None
        if not isinstance(url, str) or not url.startswith("https://"):
            raise Refused(f"{architecture}.{name}.url must be HTTPS")
        if not isinstance(digest, str) or len(digest) != 64:
            raise Refused(f"{architecture}.{name}.sha256 must be a SHA-256 digest")
        tools[name] = Tool(name=name, url=url, sha256=digest.lower())
    return tools


class Workspace:
    """The title's package work directory; it never touches a path outside it."""

    def __init__(self, work: Path) -> None:
        self.work = work
        self.downloads = work / "downloads"
        self.tools = work / "tools"
        self.appdir = work / "AppDir"

    def reset(self, path: Path) -> None:
        self.remove(path)
        path.mkdir(parents=True)

    def remove(self, path: Path) -> None:
        resolved = path.resolve()
        work = self.work.resolve()
        if resolved == work or not resolved.is_relative_to(work):
            raise Refused(f"refusing to clear package path outside {work}: {resolved}")
        if not path.exists():
            return
        if path.is_symlink() or not path.is_dir():
            raise Refused(f"refusing to clear non-directory package path {path}")
        shutil.rmtree(path)

    def download(self, tool: Tool) -> Path:
        self.downloads.mkdir(parents=True, exist_ok=True)
        destination = self.downloads / Path(urllib.request.urlparse(tool.url).path).name
        if destination.is_file() and sha256(destination) == tool.sha256:
            return destination
        destination.unlink(missing_ok=True)
        pending = destination.with_suffix(destination.suffix + ".pending")
        pending.unlink(missing_ok=True)
        try:
            with urllib.request.urlopen(tool.url, timeout=120) as response, pending.open("wb") as output:
                shutil.copyfileobj(response, output)
        except (OSError, urllib.error.URLError) as error:
            pending.unlink(missing_ok=True)
            raise Refused(f"cannot download pinned {tool.name} from {tool.url}: {error}") from error
        actual = sha256(pending)
        if actual != tool.sha256:
            pending.unlink(missing_ok=True)
            raise Mismatch(f"{tool.name} SHA-256 {actual} does not match pinned {tool.sha256}")
        pending.replace(destination)
        destination.chmod(destination.stat().st_mode | stat.S_IXUSR)
        return destination

    def unpack(self, tool: Tool, member: str) -> Path:
        """`member` of the pinned tarball, unpacked once."""
        source = self.download(tool)
        target = self.tools / tool.name
        marker = target / ".source-sha256"
        if marker.is_file() and marker.read_text(encoding="ascii").strip() == tool.sha256 \
                and (target / member).is_file():
            return target / member
        self.reset(target)
        try:
            with tarfile.open(source) as archive:
                archive.extractall(target, filter="data")
        except (OSError, tarfile.TarError) as error:
            raise Refused(f"cannot unpack pinned {tool.name}: {error}") from error
        if not (target / member).is_file():
            raise Refused(f"pinned {tool.name} holds no {member}")
        marker.write_text(tool.sha256 + "\n", encoding="ascii")
        return target / member

    def extract(self, tool: Tool) -> Path:
        """The pinned AppImage tool's AppRun, extracted once, so no FUSE is needed."""
        source = self.download(tool)
        target = self.tools / tool.name
        marker = target / ".source-sha256"
        if marker.is_file() and marker.read_text(encoding="ascii").strip() == tool.sha256 \
                and (target / "AppRun").is_file():
            return target / "AppRun"
        staging = self.tools / f"{tool.name}-extract"
        self.reset(staging)
        completed = subprocess.run([str(source), "--appimage-extract"],
                                   cwd=staging, text=True, capture_output=True, check=False)
        extracted = staging / "squashfs-root"
        if completed.returncode != 0 or not (extracted / "AppRun").is_file():
            raise Refused(f"cannot extract pinned {tool.name}: "
                          f"{(completed.stderr or completed.stdout or 'no diagnostic').strip()}")
        self.remove(target)
        extracted.replace(target)
        marker.write_text(tool.sha256 + "\n", encoding="ascii")
        self.remove(staging)
        return target / "AppRun"


def prepare_appdir(title: Title, binary: Path, space: Workspace, tools: Mapping[str, Tool]) -> None:
    linuxdeploy = space.extract(tools["linuxdeploy"])
    patchelf = space.unpack(tools["patchelf"], "bin/patchelf")
    space.reset(space.appdir)
    # The committed entry carries no Exec; the shortcut tool and this packager each write their own.
    desktop = space.work / title.entry.source.name
    desktop.write_text(desktop_shortcut.with_field(title.entry.source.read_text(encoding="utf-8"), "Exec",
                                                   binary.name),
                       encoding="utf-8")
    environment = os.environ.copy()
    environment["NO_STRIP"] = "1"
    environment["PATCHELF"] = str(patchelf)
    run_checked([str(linuxdeploy), "--appdir", str(space.appdir), "--executable", str(binary),
                 "--desktop-file", str(desktop), "--icon-file", str(title.entry.icon)],
                environment=environment)

    # The port reads its assets from <APPDIR>/usr/share/psxport (psx::config::applyPackagedDefaults).
    if not ASSETS.is_dir():
        raise Refused(f"framework assets are missing at {ASSETS}")
    shutil.copytree(ASSETS, space.appdir / "usr" / "share" / "psxport" / "assets", dirs_exist_ok=True)
    licences = space.appdir / "usr" / "share" / "licenses" / title.entry.app_id
    licences.mkdir(parents=True, exist_ok=True)
    shutil.copy2(title.licence, licences / title.licence.name)
    metainfo = space.appdir / "usr" / "share" / "metainfo"
    metainfo.mkdir(parents=True, exist_ok=True)
    if title.metainfo is not None:
        shutil.copy2(title.metainfo, metainfo / title.metainfo.name)
    else:
        (metainfo / f"{title.entry.app_id}.metainfo.xml").write_text(generated_metainfo(title, binary.name),
                                                                    encoding="utf-8")

    app_run = space.appdir / "AppRun"
    app_run.unlink(missing_ok=True)
    app_run.symlink_to(f"usr/bin/{binary.name}")
    directory_icon = space.appdir / ".DirIcon"
    directory_icon.unlink(missing_ok=True)
    directory_icon.symlink_to(title.entry.icon.name)
    audit_appdir(title, space.appdir)


def generated_metainfo(title: Title, binary: str) -> str:
    """AppStream metadata from the desktop entry, for an MIT title with no committed metainfo."""
    if not title.licence.read_text(encoding="utf-8").startswith("MIT License"):
        raise Mismatch(f"{title.licence.name} is not MIT; commit "
                       f"packaging/linux/{title.entry.app_id}.metainfo.xml naming its licence")
    text = title.entry.source.read_text(encoding="utf-8")
    name, comment = (escape(desktop_shortcut.field(text, key)) for key in ("Name", "Comment"))
    if not comment:
        raise Mismatch(f"{title.entry.source} has no Comment= for the AppStream summary")
    categories = "".join(f"    <category>{escape(category)}</category>\n"
                         for category in desktop_shortcut.field(text, "Categories").split(";") if category)
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<component type="desktop-application">\n'
            f"  <id>{title.entry.app_id}</id>\n"
            f"  <name>{name}</name>\n"
            f"  <summary>{comment}</summary>\n"
            f"  <metadata_license>CC0-1.0</metadata_license>\n"
            f"  <project_license>MIT</project_license>\n"
            f"  <description>\n"
            f"    <p>{comment}. The original game is not included: the first launch asks for the player's "
            f"own disc image.</p>\n"
            f"  </description>\n"
            f'  <launchable type="desktop-id">{title.entry.source.name}</launchable>\n'
            f"  <provides>\n    <binary>{escape(binary)}</binary>\n  </provides>\n"
            f"  <categories>\n{categories}  </categories>\n"
            f"</component>\n")


def audit_appdir(title: Title, appdir: Path) -> None:
    files = audit_content(title, appdir, "AppDir")
    validator = shutil.which("desktop-file-validate")
    if validator is not None:
        run_checked([validator, str(appdir / title.entry.source.name)])
    print(f"[appimage] content audit: {files} packaged files, 0 game files")


def parse_glibc(text: str) -> tuple[int, ...]:
    try:
        return tuple(int(part) for part in text.split("."))
    except ValueError as error:
        raise Refused(f"--max-glibc {text!r} is not a version") from error


def audit_glibc(appdir: Path, ceiling: tuple[int, ...]) -> None:
    """Refuses an AppDir whose ELF files need a newer glibc than the oldest supported distribution."""
    newest: tuple[tuple[int, ...], Path] | None = None
    scanned = 0
    for path in appdir.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as input_file:
            if input_file.read(len(ELF_MAGIC)) != ELF_MAGIC:
                continue
        scanned += 1
        completed = subprocess.run(["readelf", "--version-info", "--wide", str(path)], text=True,
                                   capture_output=True, check=False)
        if completed.returncode != 0:
            raise Refused(f"readelf cannot read {path}: {completed.stderr.strip()}")
        for match in GLIBC_REFERENCE.finditer(completed.stdout):
            version = tuple(int(part) for part in match.groups() if part is not None)
            if newest is None or version > newest[0]:
                newest = (version, path.relative_to(appdir))
    if scanned == 0:
        raise Refused("the glibc audit found no ELF files")
    if newest is not None and newest[0] > ceiling:
        raise Mismatch(f"{newest[1]} needs GLIBC_{'.'.join(map(str, newest[0]))}, newer than the "
                       f"{'.'.join(map(str, ceiling))} floor; build in tools/port/appimage-build.Containerfile")
    found = ".".join(map(str, newest[0])) if newest else "none"
    print(f"[appimage] glibc audit: {scanned} ELF files, newest GLIBC_{found}")


def update_information(title: Title, repository: str) -> str:
    owner, separator, name = repository.partition("/")
    if not separator or not owner or not name or "/" in name or "|" in repository:
        raise Refused(f"--update-repository {repository!r} is not OWNER/REPO")
    return f"gh-releases-zsync|{owner}|{name}|latest|{title.name}-*-x86_64.AppImage.zsync"


def build_appimage(title: Title, space: Workspace, output: Path, version: str, epoch: int,
                   tools: Mapping[str, Tool], update: str | None) -> Path:
    appimagetool = space.extract(tools["appimagetool"])
    runtime = space.download(tools["runtime"])
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"{title.name}-{version}-x86_64.AppImage"
    destination.unlink(missing_ok=True)
    environment = os.environ.copy()
    environment.update({"ARCH": "x86_64", "VERSION": version, "SOURCE_DATE_EPOCH": str(epoch)})
    command = [str(appimagetool), "--runtime-file", str(runtime)]
    if update is not None:
        command += ["--updateinformation", update]
    # appimagetool writes the .zsync into its working directory.
    run_checked([*command, str(space.appdir), str(destination)], environment=environment, cwd=output)
    if update is not None and not destination.with_name(destination.name + ".zsync").is_file():
        raise Refused(f"appimagetool embedded update information but wrote no {destination.name}.zsync")
    destination.chmod(destination.stat().st_mode | stat.S_IXUSR)
    verify_artifact(space, destination)
    print(f"[appimage] artifact: {destination} ({destination.stat().st_size} bytes, sha256 {sha256(destination)})")
    return destination


def verify_artifact(space: Workspace, artifact: Path) -> None:
    check = space.work / "artifact-check"
    space.reset(check)
    completed = subprocess.run([str(artifact.resolve()), "--appimage-extract"],
                               cwd=check, text=True, capture_output=True, check=False)
    extracted = check / "squashfs-root"
    if completed.returncode != 0 or not (extracted / "AppRun").exists():
        raise Refused("cannot extract the built AppImage: "
                      + (completed.stderr or completed.stdout or "no diagnostic").strip())
    run_checked([str(extracted / "AppRun"), "--help"])
    space.remove(check)
    print("[appimage] artifact check: the extracted AppRun answered --help")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="the built port")
    parser.add_argument("--version", help="release version; defaults to git describe")
    parser.add_argument("--output", type=Path, help="output directory; defaults to build/package/appimage/out")
    parser.add_argument("--max-glibc", help="refuse a package that needs a newer glibc, e.g. 2.35")
    parser.add_argument("--update-repository", metavar="OWNER/REPO",
                        help="embed GitHub-release update information and write the .zsync beside it")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="developer staging only; a release is built from a clean commit")
    args = parser.parse_args(argv)
    root = Path.cwd()
    try:
        title = resolve_title(root)
        binary = args.binary.resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise Refused(f"the port is missing or not executable at {binary}")
        version, epoch = release_version(root, args.version, args.allow_dirty)
        tools = load_tools()
        space = Workspace(root / "build" / "package" / "appimage")
        update = update_information(title, args.update_repository) if args.update_repository else None
        prepare_appdir(title, binary, space, tools)
        if args.max_glibc:
            audit_glibc(space.appdir, parse_glibc(args.max_glibc))
        build_appimage(title, space, args.output or space.work / "out", version, epoch, tools, update)
        return 0
    except Mismatch as error:
        print(f"MISMATCH: {error}", file=sys.stderr)
        return 1
    except Refused as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
