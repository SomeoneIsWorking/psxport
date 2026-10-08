"""PS-X EXE reader tests for successful and rejected images."""

from __future__ import annotations

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.formats import psx_exe


def make_image(text: bytes = b"\x11\x22\x33\x44") -> bytes:
    image = bytearray(0x800 + len(text))
    image[:8] = b"PS-X EXE"
    struct.pack_into("<II", image, 0x10, 0x80012340, 0x80020000)
    struct.pack_into("<II", image, 0x18, 0x80010000, len(text))
    struct.pack_into("<II", image, 0x30, 0x801FFF00, 0x20)
    image[0x800:] = text
    return bytes(image)


def test_load(tmp_path: Path) -> None:
    path = tmp_path / "fixture.exe"
    path.write_bytes(make_image())
    image = psx_exe.load(path)
    assert image.entry == 0x80012340
    assert image.gp == 0x80020000
    assert image.load == 0x80010000
    assert image.sp_base == 0x801FFF00
    assert image.sp_off == 0x20
    assert image.word(0x80010000) == 0x44332211


def test_rejects_bad_magic(tmp_path: Path) -> None:
    path = tmp_path / "invalid.exe"
    path.write_bytes(b"not a PS-X EXE")
    try:
        psx_exe.load(path)
    except ValueError as error:
        assert "not a PS-X EXE" in str(error)
    else:
        raise AssertionError("bad magic was accepted")


def test_load_ram(tmp_path: Path) -> None:
    path = tmp_path / "fixture.ram"
    path.write_bytes(b"RAM")
    image = psx_exe.load_ram(path)
    assert image.load == 0x80000000
    assert image.text == b"RAM"


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        test_load(root)
        test_rejects_bad_magic(root)
        test_load_ram(root)
    print("PS-X EXE reader: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
