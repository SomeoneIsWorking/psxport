"""Build a title's Android APK without game files.

    uv run --frozen python external/psxport/tools/port/android_apk.py [--release]

Run from the title root after `consumer_build.py --platform android --build build/android --target <product>`.
The title supplies only what every package reads from its committed desktop entry (`release_package.py`): the
application id, the name and the SVG icon. This owns the rest: the Gradle project (`tools/port/android/`, one
for every title), the shared android-port runtime (SDL's Java, its Activity framework, the Gradle wrapper and the
staged `libSDL3.so`/`libc++_shared.so`), the framework's assets, which the Activity unpacks into the files
directory the port treats as its package (`psx::config::Config`), and launcher icons at each density.

A default build is signed with the SDK's local debug key and is not a release. `--release` signs with the
maintainer key named by PSXPORT_ANDROID_KEYSTORE, PSXPORT_ANDROID_KEY_ALIAS, PSXPORT_ANDROID_STORE_PASSWORD and
PSXPORT_ANDROID_KEY_PASSWORD, and refuses without all four.

Exit 0: the APK was built and audited. 1: an input contradicted the release contract. 2: the build could not
be performed.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from port import app_icon, desktop_shortcut, target_platform
from port.release_package import Mismatch, Refused, Title, audit_content, release_version, resolve_title, sha256

FRAMEWORK = Path(__file__).resolve().parents[2]
TEMPLATE = Path(__file__).resolve().parent / "android"
ASSETS = FRAMEWORK / "assets"
# The prefix capabilities the port links: SDL3, SDL3_image, FreeType and fmt.
CAPABILITIES = ["sdl3", "image", "font", "format"]
EMULATOR_SERIAL = "emulator-5554"
SIGNING = ("PSXPORT_ANDROID_KEYSTORE", "PSXPORT_ANDROID_KEY_ALIAS", "PSXPORT_ANDROID_STORE_PASSWORD",
           "PSXPORT_ANDROID_KEY_PASSWORD")


def android_port_tools(checkout: Path) -> ModuleType:
    """The shared android-port CLI module, from the checkout target_platform resolved."""
    sys.path.insert(0, str(checkout / "tools"))
    import android_port  # noqa: PLC0415

    return android_port


def write_profile(work: Path, checkout: Path, prefix: Path, binary: Path) -> Path:
    """android-port's package profile for this build, with paths relative to it as its contract asks."""
    profile = work / "android-port-profile.json"

    def relative(path: Path) -> str:
        return os.path.relpath(path, work)

    profile.write_text(json.dumps({
        "schema": 1,
        "nativeDependencies": {"abi": target_platform.ANDROID_ABI, "api": target_platform.ANDROID_API,
                               "prefix": relative(prefix), "capabilities": CAPABILITIES},
        "package": {"nativeLibrary": relative(binary), "jniLibs": relative(work / "jniLibs")},
        "sharedEmulator": {"lock": relative(checkout / "coord" / "android-emulator.lock"),
                           "serial": EMULATOR_SERIAL},
    }, indent=2) + "\n", encoding="utf-8")
    return profile


def stage(title: Title, work: Path, version: str, epoch: int) -> Path:
    """The Gradle project with the title's identity, the framework's assets and the launcher icons."""
    project = work / "project"
    shutil.copytree(TEMPLATE, project, dirs_exist_ok=True)
    assets = work / "assets"
    if assets.exists():
        shutil.rmtree(assets)
    if not ASSETS.is_dir():
        raise Refused(f"framework assets are missing at {ASSETS}")
    # PsxportActivity unpacks this APK path into files/assets.
    shutil.copytree(ASSETS, assets / "psxport")
    res = work / "res"
    if res.exists():
        shutil.rmtree(res)
    app_icon.mipmaps(title.entry.icon, res, work / "icon")
    label = desktop_shortcut.field(title.entry.source.read_text(encoding="utf-8"), "Name")
    properties = {
        "applicationId": title.entry.app_id,
        "label": label,
        "versionName": version.lstrip("v"),
        # The commit time orders every build of one title.
        "versionCode": str(epoch),
        "minSdk": str(target_platform.ANDROID_API),
        "ndkVersion": target_platform.ANDROID_NDK_VERSION,
        "jniLibs": str(work / "jniLibs"),
        "assets": str(assets),
        "res": str(res),
    }
    (project / "title.properties").write_text(
        "".join(f"{key}={value.replace(chr(92), chr(92) * 2)}\n" for key, value in properties.items()), encoding="utf-8")
    return project


def assemble(project: Path, java_home: Path, release: bool) -> Path:
    task = ":app:assembleRelease" if release else ":app:assembleDebug"
    print(f"[android] gradle {task}", flush=True)
    completed = subprocess.run(["./gradlew", "--no-daemon", "--quiet", f"-Dorg.gradle.java.home={java_home}", task],
                               cwd=project, check=False)
    if completed.returncode != 0:
        raise Refused(f"gradle {task} failed ({completed.returncode})")
    kind = "release" if release else "debug"
    apks = sorted((project / "app" / "build" / "outputs" / "apk" / kind).glob(f"*-{kind}.apk"))
    if len(apks) != 1:
        raise Refused(f"expected one {kind} APK, found {[apk.name for apk in apks]}")
    return apks[0]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=Path("build/android/bin/libmain.so"), help="the built port")
    parser.add_argument("--release", action="store_true", help="sign with the maintainer key (see above)")
    parser.add_argument("--version", help="release version; defaults to git describe")
    args = parser.parse_args(argv)
    root = Path.cwd()
    work = root / "build" / "android" / "package"
    try:
        if args.release and not all(os.environ.get(name) for name in SIGNING):
            raise Refused("a release APK needs " + ", ".join(SIGNING) + "; refusing to sign it with a debug key")
        title = resolve_title(root)
        binary = args.binary.resolve()
        if not binary.is_file():
            raise Refused(f"the port is missing at {binary}; run consumer_build.py --platform android first")
        version, epoch = release_version(root, args.version, allow_dirty=not args.release)
        dependencies = target_platform.dependency_root(os.environ)
        ndk = target_platform.android_ndk(os.environ)
        prefix = target_platform.android_prefix(dependencies, os.environ, ndk)
        checkout = target_platform.android_port_checkout(dependencies, os.environ)
        shared = android_port_tools(checkout)
        shared.verify_native_entry(binary, ndk, "SDL_main")
        work.mkdir(parents=True, exist_ok=True)
        project = stage(title, work, version, epoch)
        shared.stage_gradle_runtime(prefix, project)
        shared.stage_package_runtime(shared.load_android_port_profile(write_profile(work, checkout, prefix, binary)))
        print(f"[android] content audit: {audit_content(title, work / 'assets', 'APK assets')} packaged asset files, "
              "0 game files")
        apk = assemble(project, shared.select_java_home(minimum=17, maximum=26), args.release)
        shared.inspect_apk_runtime(apk, target_platform.ANDROID_ABI)
        output = work / "out"
        output.mkdir(parents=True, exist_ok=True)
        suffix = "" if args.release else "-debug"
        destination = output / f"{title.name}-{version}-android-{target_platform.ANDROID_ABI}{suffix}.apk"
        shutil.copy2(apk, destination)
        print(f"[android] artifact: {destination} ({destination.stat().st_size} bytes, sha256 {sha256(destination)})")
        return 0
    except Mismatch as error:
        print(f"MISMATCH: {error}", file=sys.stderr)
        return 1
    except (Refused, target_platform.ToolError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
