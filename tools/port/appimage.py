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
import hashlib
import json
import os
import platform
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port import desktop_shortcut

FRAMEWORK = Path(__file__).resolve().parents[2]
TOOLS_MANIFEST = Path(__file__).resolve().with_name("appimage-tools.json")
ASSETS = FRAMEWORK / "assets"
DISC_SUFFIXES = frozenset({".chd", ".cue", ".bin", ".iso", ".img", ".pbp", ".ccd", ".sub", ".mds", ".mdf"})
PSX_EXE_MAGIC = b"PS-X EXE"
ARTIFACT_NAME_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-")


class Mismatch(RuntimeError):
    """A readable input contradicts the package contract."""


class Refused(RuntimeError):
    """The package could not be built."""


@dataclass(frozen=True)
class Tool:
    name: str
    url: str
    sha256: str


@dataclass(frozen=True)
class Title:
    """The title's package identity, read from its committed desktop entry."""

    root: Path
    entry: desktop_shortcut.Shortcut
    name: str
    metainfo: Path | None
    licence: Path
    game_hashes: frozenset[str]

    @property
    def work(self) -> Path:
        return self.root / "build" / "package" / "appimage"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for block in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise Refused(f"cannot read {path}: {error}") from error


def game_hashes(root: Path) -> frozenset[str]:
    """The SHA-256 of every game file the title's manifests identify, so the audit can refuse them."""
    hashes: set[str] = set()
    for manifest in sorted((root / "titles").glob("*/*.json")):
        values = read_json(manifest)
        if not isinstance(values, dict):
            continue
        for key in ("sha256", "source_sha256"):
            value = values.get(key)
            if isinstance(value, str) and len(value) == 64:
                hashes.add(value.lower())
    return frozenset(hashes)


def resolve_title(root: Path) -> Title:
    try:
        entry = desktop_shortcut.resolve(root)
    except desktop_shortcut.ToolError as error:
        raise Refused(str(error)) from error
    name = desktop_shortcut.field(entry.source.read_text(encoding="utf-8"), "Name")
    artifact_name = "".join(c if c in ARTIFACT_NAME_CHARACTERS else "_" for c in name.replace(" ", "_")).strip("_")
    if not artifact_name:
        raise Refused(f"{entry.source} names no usable Name=")
    metainfo = entry.source.with_name(f"{entry.app_id}.metainfo.xml")
    licences = [root / candidate for candidate in ("LICENSE", "COPYING") if (root / candidate).is_file()]
    if not licences:
        raise Refused(f"the title has no LICENSE or COPYING at {root}")
    return Title(root=root,
                 entry=entry,
                 name=artifact_name,
                 metainfo=metainfo if metainfo.is_file() else None,
                 licence=licences[0],
                 game_hashes=game_hashes(root))


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


def git_value(root: Path, arguments: Sequence[str]) -> str:
    completed = subprocess.run(["git", *arguments], cwd=root, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        raise Refused(f"git {' '.join(arguments)} failed: "
                      f"{(completed.stderr or completed.stdout or 'no diagnostic').strip()}")
    return completed.stdout.strip()


def release_version(root: Path, requested: str | None, allow_dirty: bool) -> tuple[str, int]:
    if git_value(root, ["status", "--porcelain", "--untracked-files=no"]) and not allow_dirty:
        raise Refused("a release refuses a dirty worktree; land the change first")
    version = requested or git_value(root, ["describe", "--always", "--tags", "--dirty"])
    if not version or any(character not in ARTIFACT_NAME_CHARACTERS for character in version):
        raise Refused(f"version {version!r} contains a character that is unsafe in an artifact name")
    return version, int(git_value(root, ["show", "-s", "--format=%ct", "HEAD"]))


def run_checked(command: Sequence[str], *, environment: Mapping[str, str] | None = None) -> None:
    completed = subprocess.run(list(command), text=True, capture_output=True, check=False, env=environment)
    if completed.returncode != 0:
        detail = "\n".join(output.strip() for output in (completed.stdout, completed.stderr) if output.strip())
        raise Refused(f"{' '.join(command)} failed:\n{detail or 'no diagnostic'}")


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
    if title.metainfo is not None:
        metainfo = space.appdir / "usr" / "share" / "metainfo"
        metainfo.mkdir(parents=True, exist_ok=True)
        shutil.copy2(title.metainfo, metainfo / title.metainfo.name)

    app_run = space.appdir / "AppRun"
    app_run.unlink(missing_ok=True)
    app_run.symlink_to(f"usr/bin/{binary.name}")
    directory_icon = space.appdir / ".DirIcon"
    directory_icon.unlink(missing_ok=True)
    directory_icon.symlink_to(title.entry.icon.name)
    audit_appdir(title, space.appdir)


def audit_appdir(title: Title, appdir: Path) -> None:
    """Refuses any disc image, PS-X executable or file the title's manifests identify as game content."""
    files = 0
    for path in appdir.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        files += 1
        relative = path.relative_to(appdir)
        if path.suffix.lower() in DISC_SUFFIXES:
            raise Mismatch(f"the AppDir holds a disc-image-like file {relative}")
        with path.open("rb") as input_file:
            if input_file.read(len(PSX_EXE_MAGIC)) == PSX_EXE_MAGIC:
                raise Mismatch(f"the AppDir holds a PS-X EXE at {relative}")
        if title.game_hashes and sha256(path) in title.game_hashes:
            raise Mismatch(f"the AppDir holds identified game content at {relative}")
    if files == 0:
        raise Refused("the AppDir audit scanned no files")
    validator = shutil.which("desktop-file-validate")
    if validator is not None:
        run_checked([validator, str(appdir / title.entry.source.name)])
    print(f"[appimage] content audit: {files} packaged files, 0 game files")


def build_appimage(title: Title, space: Workspace, output: Path, version: str, epoch: int,
                   tools: Mapping[str, Tool]) -> Path:
    appimagetool = space.extract(tools["appimagetool"])
    runtime = space.download(tools["runtime"])
    output.mkdir(parents=True, exist_ok=True)
    destination = output / f"{title.name}-{version}-x86_64.AppImage"
    destination.unlink(missing_ok=True)
    environment = os.environ.copy()
    environment.update({"ARCH": "x86_64", "VERSION": version, "SOURCE_DATE_EPOCH": str(epoch)})
    run_checked([str(appimagetool), "--runtime-file", str(runtime), str(space.appdir), str(destination)],
                environment=environment)
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
        space = Workspace(title.work)
        prepare_appdir(title, binary, space, tools)
        build_appimage(title, space, args.output or title.work / "out", version, epoch, tools)
        return 0
    except Mismatch as error:
        print(f"MISMATCH: {error}", file=sys.stderr)
        return 1
    except Refused as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
