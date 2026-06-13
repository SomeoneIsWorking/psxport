#!/usr/bin/env python3
"""Disassemble a region of a 2MB PSX main-RAM dump (capstone, MIPS32 LE).

Usage: disasm.py <ramdump> <start_hex> <end_hex>
Addresses are KSEG0/virtual (0x800xxxxx) or physical (0x000xxxxx); both map to
the same 2MB file offset (addr & 0x1FFFFF).
"""
import sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    data = open(path, "rb").read()
    base = start & 0x1FFFFF
    n = (end - start) & 0x1FFFFF
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    vaddr = (start & ~0x1FFFFF) | (start & 0x1FFFFF)  # keep KSEG0 bits if given
    for insn in md.disasm(data[base:base + n], start):
        print(f"  {insn.address:08X}  {insn.bytes.hex()}  {insn.mnemonic:8s} {insn.op_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
