"""Bounded libretro framebuffer ownership and explicit PNG capture."""

from __future__ import annotations

import ctypes as ct
from dataclasses import dataclass
from pathlib import Path
import struct
import zlib

MAX_WIDTH = 2048
MAX_HEIGHT = 1024
MAX_FRAME_BYTES = 16 * 1024 * 1024
PIXEL_BYTES = {0: 2, 1: 4, 2: 2}  # 0RGB1555, XRGB8888, RGB565 (native endian)


@dataclass(frozen=True)
class Framebuffer:
    width: int
    height: int
    pitch: int
    pixel_format: int
    data: bytes

    @classmethod
    def capture(cls, pointer: int, width: int, height: int,
                pitch: int, pixel_format: int) -> Framebuffer:
        if pixel_format not in PIXEL_BYTES:
            raise ValueError(f"unsupported libretro pixel format {pixel_format}")
        if not pointer or pointer == ct.c_void_p(-1).value:
            raise ValueError("software framebuffer requires a real CPU pointer")
        if not 0 < width <= MAX_WIDTH or not 0 < height <= MAX_HEIGHT:
            raise ValueError(f"frame dimensions {width}x{height} exceed the diagnostic bounds")
        if pitch < width * PIXEL_BYTES[pixel_format] or pitch * height > MAX_FRAME_BYTES:
            raise ValueError(f"invalid framebuffer stride {pitch} for {width}x{height}")
        return cls(width, height, pitch, pixel_format, ct.string_at(pointer, pitch * height))

    def rgb_rows(self) -> bytes:
        rows = bytearray()
        bpp = PIXEL_BYTES[self.pixel_format]
        word = struct.Struct("=I" if bpp == 4 else "=H")
        for y in range(self.height):
            rows.append(0)  # PNG filter: none
            for x in range(self.width):
                value = word.unpack_from(self.data, y * self.pitch + x * bpp)[0]
                if self.pixel_format == 1:
                    rgb = ((value >> 16) & 255, (value >> 8) & 255, value & 255)
                else:
                    green_bits = 6 if self.pixel_format == 2 else 5
                    red = (value >> (green_bits + 5)) & 31
                    green = (value >> 5) & ((1 << green_bits) - 1)
                    blue = value & 31
                    rgb = ((red << 3) | (red >> 2),
                           (green << (8 - green_bits)) | (green >> (2 * green_bits - 8)),
                           (blue << 3) | (blue >> 2))
                rows.extend(rgb)
        return bytes(rows)

    def save_png(self, path: Path) -> None:
        def chunk(kind: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + kind + data +
                    struct.pack(">I", zlib.crc32(kind + data)))

        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0)
        payload = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                   chunk(b"IDAT", zlib.compress(self.rgb_rows())) + chunk(b"IEND", b""))
        path.write_bytes(payload)
