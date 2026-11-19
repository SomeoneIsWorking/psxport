"""A title's application icon in each platform's container, drawn from its committed SVG.

The pinned resvg renders every size; ICO (Windows), ICNS (macOS) and Android's mipmap densities hold those PNGs
unchanged, so none needs a platform tool. resvg lands under the shared dependency root like the other pinned tools.
"""

from __future__ import annotations

import os
import stat
import struct
import subprocess
import sys
from collections.abc import Iterable
from pathlib import Path

from automation.process import ToolError
from port import release_platform
from target_platform import provision as target_platform

RESVG_RELEASE = "v0.49.0"
RESVG_ASSETS = {
    "linux": ("resvg-linux-x86_64.tar.gz", "216637e75e909e53e3098b0875cd5f7da2fdc8b4359771b4a29b539afff9d3f5"),
    "darwin": ("resvg-macos-x86_64.zip", "7c92cba55348f0254707c8d2c61e71651296981f8ccfadf8477782e91db5e171"),
}
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
# Android's launcher icon at each screen density, 48 dp.
MIPMAP_SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
# ICNS element types and the pixel size each holds.
ICNS_TYPES = ((b"icp4", 16), (b"ic11", 32), (b"icp5", 32), (b"ic12", 64), (b"ic07", 128), (b"ic13", 256),
              (b"ic08", 256), (b"ic14", 512), (b"ic09", 512), (b"ic10", 1024))


def resvg() -> Path:
    if sys.platform not in RESVG_ASSETS:
        raise ToolError(f"no pinned resvg for {sys.platform}")
    asset, digest = RESVG_ASSETS[sys.platform]
    url = f"https://github.com/linebender/resvg/releases/download/{RESVG_RELEASE}/{asset}"
    root = release_platform.dependency_root(os.environ) / f"resvg-{RESVG_RELEASE}"
    try:
        binary = target_platform.extracted(url, digest, root, "", Path("resvg")) / "resvg"
    except target_platform.ToolError as error:
        raise ToolError(f"resvg {RESVG_RELEASE}: {error}") from error
    binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
    return binary


def _rendered(svg: Path, sizes: Iterable[int], work: Path) -> dict[int, bytes]:
    renderer = resvg()
    work.mkdir(parents=True, exist_ok=True)
    pngs = {}
    for size in sorted(set(sizes)):
        out = work / f"icon_{size}.png"
        completed = subprocess.run([str(renderer), "-w", str(size), "-h", str(size), str(svg), str(out)],
                                   text=True, capture_output=True, check=False)
        if completed.returncode != 0:
            raise ToolError(f"resvg cannot render {svg} at {size}: {completed.stderr.strip()}")
        pngs[size] = out.read_bytes()
    return pngs


def ico(svg: Path, destination: Path, work: Path) -> Path:
    pngs = _rendered(svg, ICO_SIZES, work)
    offset = 6 + 16 * len(pngs)
    header = struct.pack("<HHH", 0, 1, len(pngs))
    entries = b""
    for size, data in pngs.items():
        # 256 is written as 0.
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(header + entries + b"".join(pngs.values()))
    return destination


def icns(svg: Path, destination: Path, work: Path) -> Path:
    pngs = _rendered(svg, (size for _, size in ICNS_TYPES), work)
    elements = b"".join(kind + struct.pack(">I", 8 + len(pngs[size])) + pngs[size] for kind, size in ICNS_TYPES)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(b"icns" + struct.pack(">I", 8 + len(elements)) + elements)
    return destination


def mipmaps(svg: Path, res: Path, work: Path) -> Path:
    """`res/mipmap-<density>/ic_launcher.png` for every density."""
    pngs = _rendered(svg, MIPMAP_SIZES.values(), work)
    for density, size in MIPMAP_SIZES.items():
        directory = res / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "ic_launcher.png").write_bytes(pngs[size])
    return res
