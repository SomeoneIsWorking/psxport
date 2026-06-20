"""PS-X EXE loader for the static recompiler.

A PS-X EXE is a 0x800-byte header followed by the text image. The header gives the
entry PC, the load (destination) address, the text size, and the initial SP/GP.
We load the text as a flat bytes blob keyed by its virtual base so the recompiler's
addresses match docs/journal.md and the Ghidra decomp (KSEG0, based at 0x80010000).
"""
from __future__ import annotations
import struct
from dataclasses import dataclass


@dataclass
class PsxExe:
    entry: int          # initial PC
    gp: int             # initial GP
    load: int           # text destination (virtual) address
    text_size: int      # bytes of text
    sp_base: int        # initial SP
    sp_off: int
    text: bytes         # text image (text_size bytes)

    @property
    def text_end(self) -> int:
        return self.load + self.text_size

    def word(self, vaddr: int) -> int:
        """Little-endian 32-bit word at a virtual address inside the text image."""
        off = vaddr - self.load
        if off < 0 or off + 4 > len(self.text):
            raise IndexError(f"vaddr 0x{vaddr:08X} outside text "
                             f"[0x{self.load:08X},0x{self.text_end:08X})")
        return struct.unpack_from("<I", self.text, off)[0]


def load_ram(path: str) -> PsxExe:
    """Load a raw 2MB main-RAM dump (e.g. scratch/bin/tomba2/ram_menu.bin) so overlay code that is
    NOT in MAIN.EXE (DEMO/GAME stage handlers at 0x80106xxx) can be disassembled. KSEG0-based: a
    virtual address 0x80xxxxxx indexes the dump at (vaddr - 0x80000000)."""
    d = open(path, "rb").read()
    return PsxExe(0, 0, 0x80000000, len(d), 0, 0, d)


def load(path: str) -> PsxExe:
    d = open(path, "rb").read()
    if d[:8] != b"PS-X EXE":
        raise ValueError(f"{path}: not a PS-X EXE (magic={d[:8]!r})")
    entry, gp = struct.unpack_from("<II", d, 0x10)
    load_addr, tsize = struct.unpack_from("<II", d, 0x18)
    sp_base, sp_off = struct.unpack_from("<II", d, 0x30)
    text = d[0x800:0x800 + tsize]
    if len(text) != tsize:
        raise ValueError(f"{path}: truncated text ({len(text)} != {tsize})")
    return PsxExe(entry, gp, load_addr, tsize, sp_base, sp_off, text)
