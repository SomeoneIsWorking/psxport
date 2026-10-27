"""Builds the framework test program: one freestanding C file compiled for mipsel and linked into a PS-X EXE.

There is no MIPS linker on the build hosts, so this links the single object itself: alloc sections in order from
the load address, .bss zero-filled inside the text, and the four o32 relocations a -G0 non-PIC object carries.
It also writes the C++ identity header the product authenticates the EXE against.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from formats.psx_exe import MAGIC, TEXT_FILE_OFFSET  # noqa: E402

LOAD_ADDRESS = 0x80010000
STACK_ADDRESS = 0x801FFF00
SERIAL = "PSXT_000.01"

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
# MIPS ABI notes the loader never reads.
SHT_MIPS_REGINFO = 0x70000006
SHT_MIPS_ABIFLAGS = 0x7000002A
SHN_UNDEF = 0
SHN_ABS = 0xFFF1

R_MIPS_NONE = 0
R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
R_MIPS_JALR = 37


@dataclass
class Section:
    index: int
    name: str
    kind: int
    flags: int
    offset: int
    size: int
    align: int
    link: int
    info: int
    address: int = 0


def compile_object(clang: str, source: Path, out: Path) -> None:
    subprocess.run(
        [clang, "--target=mipsel-unknown-none-elf", "-march=mips1", "-mabi=32", "-mno-abicalls", "-fno-pic",
         "-G0", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-Wall", "-Werror",
         "-Wno-unused-command-line-argument", "-c", str(source), "-o", str(out)],
        check=True)


def read_sections(elf: bytes) -> list[Section]:
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise SystemExit("not a 32-bit little-endian ELF object")
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    names = raw[shstrndx][4]
    sections = []
    for index, (name, kind, flags, _, offset, size, link, info, align, _) in enumerate(raw):
        label = elf[names + name:elf.index(b"\0", names + name)].decode()
        sections.append(Section(index, label, kind, flags, offset, size, max(align, 1), link, info))
    return sections


def layout(sections: list[Section]) -> tuple[list[Section], int]:
    def rank(section: Section) -> tuple[int, str]:
        if section.name.startswith(".text.0start"):
            return (0, section.name)
        if section.kind == SHT_NOBITS:
            return (3, section.name)
        if section.name.startswith(".text"):
            return (1, section.name)
        return (2, section.name)

    placed = sorted((s for s in sections if s.flags & SHF_ALLOC and s.size and
                     s.kind not in (SHT_MIPS_REGINFO, SHT_MIPS_ABIFLAGS)), key=rank)
    if not placed or not placed[0].name.startswith(".text.0start"):
        raise SystemExit("the object has no .text.0start entry section")
    address = LOAD_ADDRESS
    for section in placed:
        address = (address + section.align - 1) & ~(section.align - 1)
        section.address = address
        address += section.size
    return placed, address - LOAD_ADDRESS


def symbols(elf: bytes, sections: list[Section]) -> list[int]:
    table = next(s for s in sections if s.kind == SHT_SYMTAB)
    strings = sections[table.link]
    values = []
    for i in range(table.size // 16):
        name, value, _, _, _, shndx = struct.unpack_from("<IIIBBH", elf, table.offset + i * 16)
        if i == 0:
            values.append(0)
        elif shndx == SHN_UNDEF:
            label = elf[strings.offset + name:elf.index(b"\0", strings.offset + name)].decode()
            raise SystemExit(f"undefined symbol {label}")
        elif shndx == SHN_ABS:
            values.append(value)
        else:
            values.append(sections[shndx].address + value)
    return values


def relocate(elf: bytes, sections: list[Section], image: bytearray, values: list[int]) -> None:
    def word(at: int) -> int:
        return struct.unpack_from("<I", image, at)[0]

    def put(at: int, value: int) -> None:
        struct.pack_into("<I", image, at, value & 0xFFFFFFFF)

    for rel in (s for s in sections if s.kind in (SHT_REL, SHT_RELA)):
        target = sections[rel.info]
        if not target.flags & SHF_ALLOC or target.kind == SHT_NOBITS:
            continue
        entry = 8 if rel.kind == SHT_REL else 12
        pending: list[int] = []
        for i in range(rel.size // entry):
            offset, info = struct.unpack_from("<II", elf, rel.offset + i * entry)
            explicit = struct.unpack_from("<i", elf, rel.offset + i * entry + 8)[0] if rel.kind == SHT_RELA else None
            kind, symbol = info & 0xFF, info >> 8
            at = target.address - LOAD_ADDRESS + offset
            s = values[symbol]
            if kind in (R_MIPS_NONE, R_MIPS_JALR):
                continue
            if kind == R_MIPS_32:
                put(at, (explicit if explicit is not None else word(at)) + s)
            elif kind == R_MIPS_26:
                addend = explicit if explicit is not None else (word(at) & 0x3FFFFFF) << 2
                put(at, (word(at) & 0xFC000000) | (((s + addend) >> 2) & 0x3FFFFFF))
            elif kind == R_MIPS_HI16:
                pending.append(at)
            elif kind == R_MIPS_LO16:
                low = word(at) & 0xFFFF
                low_addend = explicit if explicit is not None else low - 0x10000 if low & 0x8000 else low
                for hi_at in pending:
                    full = s + ((word(hi_at) & 0xFFFF) << 16 if explicit is None else 0) + low_addend
                    put(hi_at, (word(hi_at) & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
                pending.clear()
                put(at, (word(at) & 0xFFFF0000) | ((s + low_addend) & 0xFFFF))
            else:
                raise SystemExit(f"relocation type {kind} in {rel.name} is not handled")
        if pending:
            raise SystemExit(f"{rel.name}: a HI16 without its LO16")


def psx_exe(text: bytes) -> bytes:
    size = (len(text) + TEXT_FILE_OFFSET - 1) // TEXT_FILE_OFFSET * TEXT_FILE_OFFSET
    header = bytearray(TEXT_FILE_OFFSET)
    header[:8] = MAGIC
    struct.pack_into("<IIII", header, 0x10, LOAD_ADDRESS, 0, LOAD_ADDRESS, size)
    struct.pack_into("<II", header, 0x30, STACK_ADDRESS, 0)
    header[0x4C:0x4C + 23] = b"psxport test program  \0"
    return bytes(header) + text + bytes(size - len(text))


def identity_header(exe: bytes, root: Path) -> str:
    text_size = struct.unpack_from("<I", exe, 0x1C)[0]
    return (
        "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n#include <string_view>\n\n"
        "// Generated by tools/testprog/build_testprog.py from the built test program.\n"
        "namespace psx::testprog::identity {\n\n"
        f"inline constexpr std::string_view kSerial = \"{SERIAL}\";\n"
        f"inline constexpr std::string_view kProvisioningRoot = \"{root.as_posix()}\";\n"
        f"inline constexpr std::string_view kExecutableSha256 = \"{hashlib.sha256(exe).hexdigest()}\";\n"
        f"inline constexpr std::size_t kExecutableBytes = {len(exe)}u;\n"
        f"inline constexpr std::uint32_t kEntry = 0x{LOAD_ADDRESS:08X}u;\n"
        f"inline constexpr std::uint32_t kTextAddress = 0x{LOAD_ADDRESS:08X}u;\n"
        f"inline constexpr std::uint32_t kTextBytes = 0x{text_size:X}u;\n"
        f"inline constexpr std::uint32_t kStackAddress = 0x{STACK_ADDRESS:08X}u;\n\n"
        "} // namespace psx::testprog::identity\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--object", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True, help="provisioning root; the EXE goes to psxtest/<serial>")
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()
    args.object.parent.mkdir(parents=True, exist_ok=True)
    compile_object(args.clang, args.source, args.object)
    elf = args.object.read_bytes()
    sections = read_sections(elf)
    placed, size = layout(sections)
    image = bytearray(size)
    for section in placed:
        if section.kind != SHT_NOBITS:
            start = section.address - LOAD_ADDRESS
            image[start:start + section.size] = elf[section.offset:section.offset + section.size]
    relocate(elf, sections, image, symbols(elf, sections))
    exe = psx_exe(bytes(image))
    executable = args.root / "psxtest" / SERIAL
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.write_bytes(exe)
    header = identity_header(exe, args.root.resolve())
    args.header.parent.mkdir(parents=True, exist_ok=True)
    if not args.header.exists() or args.header.read_text() != header:
        args.header.write_text(header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
