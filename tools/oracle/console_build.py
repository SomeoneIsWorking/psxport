"""Build the pinned full Beetle core in one disposable build owner, never in the vendor tree."""

from __future__ import annotations

from contextlib import contextmanager
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "oracle-console"
SCRATCH = ROOT / "scratch" / "oracle-console"
VENDOR = ROOT / "vendor" / "beetle-psx"
BUILD_SCHEMA = 2


def file_sha256(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def library_name() -> str:
    suffix = {"linux": ".so", "darwin": ".dylib"}.get(sys.platform)
    if suffix is None:
        raise ValueError(f"full-console diagnostic builder does not support {sys.platform}; "
                         "a platform build/ABI qualification is required")
    return "mednafen_psx_libretro" + suffix


def git(*arguments: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(["git", *arguments], cwd=cwd, text=True).strip()


def source_identity() -> dict:
    record = git("ls-files", "--stage", "vendor/beetle-psx").split()
    if len(record) != 4 or record[0] != "160000":
        raise ValueError("vendor/beetle-psx has no exact recorded gitlink")
    revision = record[1]
    if git("rev-parse", "HEAD", cwd=VENDOR) != revision:
        raise ValueError(f"vendor checkout differs from recorded Beetle revision {revision}")
    return {"schema": BUILD_SCHEMA, "vendor_revision": revision,
            "gte_state_sha256": file_sha256(ROOT / "runtime/psx/gte_state.h"),
            "system": sys.platform, "machine": platform.machine(),
            "software_renderer": True, "mednafen_interpreter": True, "lightrec": False,
            "pc_observer_abi": 1}


@contextmanager
def activity_lock():
    """Serialize this singleton core's builds, staging, and runs across independent tool processes."""
    import fcntl

    BUILD.mkdir(parents=True, exist_ok=True)
    with (BUILD / "activity.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("another console-oracle build/run owns the activity lock") from error
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def extract_source(destination: Path, revision: str) -> None:
    archive = subprocess.check_output(["git", "archive", revision], cwd=VENDOR)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        members = source.getmembers()
        # Validate every target before publishing any source. This is a build export, not a checkout.
        for member in members:
            relative = PurePosixPath(member.name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"unsafe pinned-source path {member.name}")
            if not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError(f"unsupported pinned-source entry {member.name}")
            if member.issym():
                target = (destination / member.name).parent / member.linkname
                if not target.resolve().is_relative_to(destination.resolve()):
                    raise ValueError(f"source symlink escapes build export: {member.name}")
        for member in members:
            path = destination / member.name
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to(member.linkname)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                content = source.extractfile(member)
                if content is None:
                    raise ValueError(f"missing archived source {member.name}")
                with content, path.open("wb") as output:
                    shutil.copyfileobj(content, output)
                path.chmod(member.mode & 0o777)
    required = ("Makefile", "Makefile.common", "libretro.c", "deps/openbios/openbios.bin.h",
                "deps/libchdr/src/libchdr_chd.c", "mednafen/psx/cpu.c", "mednafen/psx/gpu.c",
                "mednafen/psx/pc_observer.c", "mednafen/psx/pc_observer.h")
    missing = [name for name in required if not (destination / name).is_file()]
    if missing:
        raise ValueError("pinned full-core sub-inputs missing: " + ", ".join(missing))


def build_core(cc: str, cxx: str, jobs: int) -> Path:
    name = library_name()
    if not 1 <= jobs <= 64:
        raise ValueError("build jobs must be between 1 and 64")
    required_tools = ("git", "make", "pkg-config", cc, cxx)
    missing = [tool for tool in required_tools if shutil.which(tool) is None]
    if missing:
        raise ValueError("missing native build tools: " + ", ".join(missing))
    subprocess.run(["pkg-config", "--exists", "zlib"], check=True)
    identity = source_identity()
    identity["cc"] = subprocess.check_output([cc, "--version"], text=True).splitlines()[0]
    identity["cxx"] = subprocess.check_output([cxx, "--version"], text=True).splitlines()[0]
    stamp = BUILD / "source.json"
    source = BUILD / "core"
    if stamp.exists() and json.loads(stamp.read_text()) != identity:
        # Only this named export is disposable; the activity lock excludes a live core using it.
        if source.is_symlink() or source.resolve() != BUILD.resolve() / "core":
            raise ValueError("refusing unexpected oracle core build path")
        shutil.rmtree(source)
        stamp.unlink()
    if not stamp.exists():
        if source.exists():
            raise ValueError(f"incomplete source export requires inspection: {source}")
        source.mkdir()
        extract_source(source, identity["vendor_revision"])
        stamp.write_text(json.dumps(identity, indent=2) + "\n")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    command = ["make", "-C", str(source), f"-j{jobs}", f"CC={cc}", f"CXX={cxx}",
               "HAVE_HW=0", "HAVE_OPENGL=0", "HAVE_VULKAN=0", "HAVE_LIGHTREC=0",
               "HAVE_CHD=1", "PSX_PC_OBSERVER=1", "SYSTEM_ZLIB=1", "LINK_STATIC_LIBCPLUSPLUS=0",
               f"TARGET={name}", f"GIT_VERSION={identity['vendor_revision']}",
               "EXTRA_INCLUDES=-I" + shlex.quote(str(ROOT / "runtime/psx"))]
    with (SCRATCH / "build.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    library = source / name
    if not library.is_file():
        raise ValueError(f"full core build produced no {library}")
    manifest = {**identity, "library_sha256": file_sha256(library)}
    (BUILD / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return library


def verified_library() -> tuple[Path, dict]:
    library = BUILD / "core" / library_name()
    manifest_path = BUILD / "manifest.json"
    if not library.is_file() or not manifest_path.is_file():
        raise ValueError("full-console core is missing; run console.py build first")
    manifest = json.loads(manifest_path.read_text())
    expected = source_identity()
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise ValueError("full-console core provenance changed; rebuild its exact pinned inputs")
    if manifest.get("library_sha256") != file_sha256(library):
        raise ValueError("full-console core binary differs from its build manifest")
    return library, manifest
