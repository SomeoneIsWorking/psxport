"""Strict PS-X EXE and raw-RAM image readers."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"PS-X EXE"
# The header is 0x800 bytes and the text always follows it.
TEXT_FILE_OFFSET = 0x800
_MIN_HEADER = 0x38


@dataclass(frozen=True)
class PsxExeHeader:
    entry: int
    gp: int
    load: int
    text_size: int
    sp_base: int
    sp_off: int
    file_size: int


@dataclass(frozen=True)
class PsxExe:
    entry: int
    gp: int
    load: int
    text_size: int
    sp_base: int
    sp_off: int
    text: bytes

    @property
    def text_end(self) -> int:
        return self.load + self.text_size

    def word(self, virtual_address: int) -> int:
        offset = virtual_address - self.load
        if offset < 0 or offset + 4 > len(self.text):
            raise IndexError(
                f"vaddr 0x{virtual_address:08X} outside text "
                f"[0x{self.load:08X},0x{self.text_end:08X})"
            )
        return struct.unpack_from("<I", self.text, offset)[0]


def parse_header(data: bytes, file_size: int, source: str | Path) -> PsxExeHeader:
    """The header fields of a PS-X EXE whose first bytes are ``data``; ValueError if it is not one."""
    if len(data) < _MIN_HEADER:
        raise ValueError(f"{source}: {len(data)} bytes, too short to hold a PS-X EXE header")
    if data[:8] != MAGIC:
        raise ValueError(f"{source}: not a PS-X EXE (magic={data[:8]!r})")
    entry, gp = struct.unpack_from("<II", data, 0x10)
    load_address, text_size = struct.unpack_from("<II", data, 0x18)
    sp_base, sp_offset = struct.unpack_from("<II", data, 0x30)
    return PsxExeHeader(entry, gp, load_address, text_size, sp_base, sp_offset, file_size)


def read_header(path: str | Path) -> PsxExeHeader:
    image_path = Path(path)
    with image_path.open("rb") as handle:
        head = handle.read(TEXT_FILE_OFFSET)
    return parse_header(head, image_path.stat().st_size, image_path)


def load_ram(path: str | Path) -> PsxExe:
    data = Path(path).read_bytes()
    return PsxExe(0, 0, 0x80000000, len(data), 0, 0, data)


def load(path: str | Path) -> PsxExe:
    image_path = Path(path)
    data = image_path.read_bytes()
    header = parse_header(data, len(data), image_path)
    text = data[TEXT_FILE_OFFSET : TEXT_FILE_OFFSET + header.text_size]
    if len(text) != header.text_size:
        raise ValueError(f"{image_path}: truncated text ({len(text)} != {header.text_size})")
    return PsxExe(header.entry, header.gp, header.load, header.text_size, header.sp_base,
                  header.sp_off, text)
