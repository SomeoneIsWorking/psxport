"""Install the Ghidra release and PyGhidra environment the decompiler pipeline runs.

    uv run --frozen python tools/decomp/install.py

Downloads the official Ghidra 12.0.4 release into its own empty scratch directory, checks its SHA-256,
extracts it to the pipeline's default home and builds PyGhidra's virtual environment from the wheels
the release ships, at the path :func:`headless.resolve_interpreter` looks in. A JDK 21 or newer must
already be on PATH or in JAVA_HOME. Does nothing when both are already present.
"""

from __future__ import annotations

import hashlib
import logging
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from decomp import headless  # noqa: E402

RELEASE_URL = (
    "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.0.4_build/"
    "ghidra_12.0.4_PUBLIC_20260303.zip"
)
RELEASE_SHA256 = "c3b458661d69e26e203d739c0c82d143cc8a4a29d9e571f099c2cf4bda62a120"
DOWNLOAD_DIR = TOOLS.parent / "scratch" / "ghidra-download"


def venv_dir(ghidra_home: Path) -> Path:
    return Path.home() / ".config" / "ghidra" / ghidra_home.name / "venv"


def download(target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with urllib.request.urlopen(RELEASE_URL) as response, target.open("wb") as output:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            output.write(chunk)
    if digest.hexdigest() != RELEASE_SHA256:
        raise RuntimeError(f"{RELEASE_URL} has SHA-256 {digest.hexdigest()}, expected {RELEASE_SHA256}")


def extract(archive: Path, ghidra_home: Path) -> None:
    with zipfile.ZipFile(archive) as release:
        top = {Path(name).parts[0] for name in release.namelist()}
        if top != {ghidra_home.name}:
            raise RuntimeError(f"{archive.name} unpacks to {sorted(top)}, not {ghidra_home.name}")
        release.extractall(ghidra_home.parent)
        for member in release.infolist():
            mode = member.external_attr >> 16
            if mode:
                (ghidra_home.parent / member.filename).chmod(mode & 0o777)


def build_venv(ghidra_home: Path) -> None:
    wheels = ghidra_home / "Ghidra" / "Features" / "PyGhidra" / "pypkg" / "dist"
    environment = venv_dir(ghidra_home)
    subprocess.run([sys.executable, "-m", "venv", str(environment)], check=True)
    python = environment / "bin" / "python3"
    bundled = sorted(wheels.glob("pyghidra-*-py3-none-any.whl"))
    if len(bundled) != 1:
        raise RuntimeError(f"expected one bundled pyghidra wheel in {wheels}, found {len(bundled)}")
    # The bundled wheel matches this install; jpype comes from the bundle when it has this Python's build.
    subprocess.run([str(python), "-m", "pip", "install", "-f", str(wheels), str(bundled[0])], check=True)


def require_jdk() -> None:
    java = shutil.which("java")
    if java is None:
        raise RuntimeError(
            "no java on PATH; Ghidra 12 needs JDK 21 or newer: Fedora `sudo dnf install java-21-openjdk-devel`, "
            "Debian/Ubuntu `sudo apt install default-jdk-headless`, macOS `brew install openjdk@21`"
        )


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="[ghidra-install] %(message)s")
    ghidra_home = headless.DEFAULT_GHIDRA_HOME
    try:
        require_jdk()
        if not (ghidra_home / "Ghidra").is_dir():
            archive = DOWNLOAD_DIR / Path(RELEASE_URL).name
            logging.info("downloading %s", RELEASE_URL)
            download(archive)
            logging.info("extracting to %s", ghidra_home)
            extract(archive, ghidra_home)
        if not (venv_dir(ghidra_home) / "bin" / "python3").is_file():
            logging.info("building PyGhidra environment at %s", venv_dir(ghidra_home))
            build_venv(ghidra_home)
        logging.info("PyGhidra interpreter: %s", headless.resolve_interpreter(ghidra_home))
    except (OSError, RuntimeError, subprocess.CalledProcessError, headless.GhidraRefusal) as error:
        logging.error("REFUSED: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
