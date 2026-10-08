"""Strict PS-X EXE and raw-RAM image readers."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


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


def load_ram(path: str | Path) -> PsxExe:
    data = Path(path).read_bytes()
    return PsxExe(0, 0, 0x80000000, len(data), 0, 0, data)


def load(path: str | Path) -> PsxExe:
    image_path = Path(path)
    data = image_path.read_bytes()
    if data[:8] != b"PS-X EXE":
        raise ValueError(f"{image_path}: not a PS-X EXE (magic={data[:8]!r})")
    entry, gp = struct.unpack_from("<II", data, 0x10)
    load_address, text_size = struct.unpack_from("<II", data, 0x18)
    sp_base, sp_offset = struct.unpack_from("<II", data, 0x30)
    text = data[0x800 : 0x800 + text_size]
    if len(text) != text_size:
        raise ValueError(f"{image_path}: truncated text ({len(text)} != {text_size})")
    return PsxExe(entry, gp, load_address, text_size, sp_base, sp_offset, text)
