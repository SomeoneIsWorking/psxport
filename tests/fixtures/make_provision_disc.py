"""Build the provisioner's fixture disc: a one-track Mode 2 CHD holding a tiny PS-X EXE.

Writes `tests/fixtures/provision_disc/SLUS_000.01` (the executable) and `provision.chd` (an ISO9660
volume with that file at `BOOT/SLUS_000.01;1`). Both are committed; rerun this only to change them.
Needs `mkisofs` and `chdman` on PATH.

    uv run --frozen python tests/fixtures/make_provision_disc.py
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "fixtures" / "provision_disc"
WORK = ROOT / "scratch" / "provision_disc_fixture"

TEXT_ADDRESS = 0x80010000
TEXT_SIZE = 0x800
STACK_ADDRESS = 0x801FFFF0
JR_RA = 0x03E00008
SECTOR = 2048
RAW_SECTOR = 2352


def executable() -> bytes:
    header = bytearray(0x800)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, TEXT_ADDRESS)
    struct.pack_into("<I", header, 0x18, TEXT_ADDRESS)
    struct.pack_into("<I", header, 0x1C, TEXT_SIZE)
    struct.pack_into("<I", header, 0x30, STACK_ADDRESS)
    text = bytearray(TEXT_SIZE)
    struct.pack_into("<I", text, 0, JR_RA)
    return bytes(header + text)


def bcd(value: int) -> int:
    return ((value // 10) << 4) | (value % 10)


def mode2_form1(iso: bytes) -> bytes:
    raw = bytearray()
    for lba in range(len(iso) // SECTOR):
        address = lba + 150
        minute, rest = divmod(address, 75 * 60)
        second, frame = divmod(rest, 75)
        raw += b"\x00" + b"\xff" * 10 + b"\x00"
        raw += bytes([bcd(minute), bcd(second), bcd(frame), 2])
        raw += b"\x00\x00\x08\x00\x00\x00\x08\x00"
        raw += iso[lba * SECTOR : (lba + 1) * SECTOR]
        raw += b"\x00" * (RAW_SECTOR - 24 - SECTOR)
    return bytes(raw)


# The package that provides each tool, per platform.
INSTALL = {
    "mkisofs": (
        "Fedora: sudo dnf install xorriso; Debian/Ubuntu: sudo apt install genisoimage; "
        "macOS: brew install cdrtools"
    ),
    "chdman": (
        "Fedora: sudo dnf install mame-tools; Debian/Ubuntu: sudo apt install mame-tools; "
        "macOS: brew install rom-tools"
    ),
}


def main() -> int:
    for tool, install in INSTALL.items():
        if shutil.which(tool) is None:
            print(f"{tool} is not on PATH. Install it with {install}", file=sys.stderr)
            return 1
    tree = WORK / "tree"
    (tree / "BOOT").mkdir(parents=True, exist_ok=True)
    exe = executable()
    (tree / "BOOT" / "SLUS_000.01").write_bytes(exe)
    iso = WORK / "provision.iso"
    subprocess.run(["mkisofs", "-quiet", "-V", "PROVISION", "-o", str(iso), str(tree)], check=True)
    bin_path = WORK / "provision.bin"
    bin_path.write_bytes(mode2_form1(iso.read_bytes()))
    cue = WORK / "provision.cue"
    cue.write_text('FILE "provision.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["chdman", "createcd", "-f", "-i", str(cue), "-o", str(OUT / "provision.chd")], check=True)
    (OUT / "SLUS_000.01").write_bytes(exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
