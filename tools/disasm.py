#!/usr/bin/env python3
"""Disassemble an aligned, end-exclusive range of an exact 2 MiB PSX RAM dump.

Run through the locked environment:
  uv run --frozen python tools/disasm.py RAM_DUMP START_HEX END_HEX
Addresses may be physical, KSEG0 or KSEG1 within the first 2 MiB. Every requested
word is visited; any word Capstone cannot decode is reported and makes the run fail.
Capstone MIPS32 decoding is diagnostic text, not proof of PSX instruction validity.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN

RAM_BYTES = 2 * 1024 * 1024
RAM_BASES = (0, 0x80000000, 0xA0000000)


def ram_range(start: int, end: int) -> tuple[int, int]:
    if start % 4 or end % 4:
        raise ValueError("start and end must be 4-byte aligned")
    if end <= start:
        raise ValueError("range must be nonempty and increasing; end is exclusive")
    for base in RAM_BASES:
        if base <= start < end <= base + RAM_BYTES:
            return start - base, end - base
    raise ValueError("range must remain within one physical/KSEG0/KSEG1 2 MiB RAM mapping")


def disassemble(path: Path, start: int, end: int) -> int:
    begin, finish = ram_range(start, end)
    with path.open("rb") as source:
        data = source.read(RAM_BYTES + 1)
    if len(data) != RAM_BYTES:
        raise ValueError(f"RAM dump must contain exactly {RAM_BYTES} bytes; read {len(data)}")
    decoder = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    requested = (finish - begin) // 4
    decoded = 0
    for offset in range(begin, finish, 4):
        address = start + offset - begin
        word = data[offset:offset + 4]
        instruction = next(decoder.disasm(word, address, count=1), None)
        if instruction is None or instruction.size != 4:
            print(f"  {address:08X}  {word.hex()}  UNKNOWN raw=0x{int.from_bytes(word, 'little'):08X}; "
                  "Capstone cannot decode this complete word")
            continue
        decoded += 1
        print(f"  {address:08X}  {word.hex()}  {instruction.mnemonic:8s} {instruction.op_str}")
    print(f"scanned {requested}/{requested} words; decoded {decoded}/{requested} words; "
          f"unknown {requested - decoded}; " + ("complete" if decoded == requested else "REFUSED incomplete decode"))
    return int(decoded != requested)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ramdump", type=Path)
    parser.add_argument("start_hex", type=lambda value: int(value, 16))
    parser.add_argument("end_hex", type=lambda value: int(value, 16))
    args = parser.parse_args()
    try:
        return disassemble(args.ramdump, args.start_hex, args.end_hex)
    except (OSError, ValueError) as error:
        print(f"REFUSED: scanned 0 words; decoded 0 words; {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
