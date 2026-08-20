"""PS-X EXE loader tests, including file ownership on success and refusal paths."""

from __future__ import annotations

import io
import struct
import sys
from pathlib import Path
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent))
import psexe  # noqa: E402


class Checks:
    def __init__(self) -> None:
        self.count = 0

    def equal(self, actual: object, expected: object) -> None:
        self.count += 1
        if actual != expected:
            raise AssertionError(f"check {self.count}: {actual!r} != {expected!r}")

    def true(self, value: object) -> None:
        self.count += 1
        if not value:
            raise AssertionError(f"check {self.count}: expected truthy value, got {value!r}")


def psx_exe(text: bytes = b"\x11\x22\x33\x44") -> bytes:
    image = bytearray(0x800 + len(text))
    image[:8] = b"PS-X EXE"
    struct.pack_into("<II", image, 0x10, 0x80012340, 0x80020000)
    struct.pack_into("<II", image, 0x18, 0x80010000, len(text))
    struct.pack_into("<II", image, 0x30, 0x801FFF00, 0x20)
    image[0x800:] = text
    return bytes(image)


def test_load_closes_success(checks: Checks) -> None:
    stream = io.BytesIO(psx_exe())
    with patch("builtins.open", return_value=stream):
        exe = psexe.load("fixture.exe")
    checks.true(stream.closed)
    checks.equal(exe.entry, 0x80012340)
    checks.equal(exe.gp, 0x80020000)
    checks.equal(exe.load, 0x80010000)
    checks.equal(exe.sp_base, 0x801FFF00)
    checks.equal(exe.sp_off, 0x20)
    checks.equal(exe.text, b"\x11\x22\x33\x44")


def test_load_closes_refusal(checks: Checks) -> None:
    stream = io.BytesIO(b"not a PS-X EXE")
    refused = False
    with patch("builtins.open", return_value=stream):
        try:
            psexe.load("invalid.exe")
        except ValueError as error:
            refused = "not a PS-X EXE" in str(error)
    checks.true(refused)
    checks.true(stream.closed)


def test_load_ram_closes_success(checks: Checks) -> None:
    stream = io.BytesIO(b"RAM")
    with patch("builtins.open", return_value=stream):
        exe = psexe.load_ram("fixture.ram")
    checks.true(stream.closed)
    checks.equal(exe.load, 0x80000000)
    checks.equal(exe.text, b"RAM")


def main() -> int:
    checks = Checks()
    test_load_closes_success(checks)
    test_load_closes_refusal(checks)
    test_load_ram_closes_success(checks)
    print(f"psexe loader: PASS ({checks.count} checks; success + refusal ownership)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
