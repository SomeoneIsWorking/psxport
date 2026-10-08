"""Bounded libretro framebuffer ownership and explicit PNG capture."""

from __future__ import annotations

import ctypes as ct
from array import array
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import sys
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


class CaptureHashes:
    """One explicit observation window; hashes own copied bytes, never emulator state."""

    def __init__(self):
        self.ram = hashlib.sha256()
        self.video = hashlib.sha256()
        self.audio = hashlib.sha256()
        self.fields = 0
        self.audio_frames = 0

    def audio_sample(self, left: int, right: int) -> None:
        self.audio.update(struct.pack("<hh", left, right))
        self.audio_frames += 1

    def audio_batch(self, native_bytes: bytes) -> None:
        if len(native_bytes) % 4:
            raise ValueError("audio hash requires complete stereo int16 frames")
        samples = array("h")
        samples.frombytes(native_bytes)
        if sys.byteorder != "little":
            samples.byteswap()
        self.audio.update(samples.tobytes())
        self.audio_frames += len(native_bytes) // 4

    def field(self, ram: bytes, framebuffer: Framebuffer) -> None:
        if len(ram) != 0x200000:
            raise ValueError("RAM hash requires the complete 2 MiB console RAM")
        self.ram.update(ram)
        # Canonical RGB excludes pitch padding, unused pixel bits and native endianness.
        self.video.update(struct.pack("<II", framebuffer.width, framebuffer.height))
        self.video.update(framebuffer.rgb_rows())
        self.fields += 1

    def status(self) -> dict:
        return {"fields": self.fields, "audio_frames": self.audio_frames,
                "ram_sha256": self.ram.hexdigest(), "frame_sha256": self.video.hexdigest(),
                "audio_sha256": self.audio.hexdigest(),
                "complete": self.fields > 0 and self.audio_frames > 0}
