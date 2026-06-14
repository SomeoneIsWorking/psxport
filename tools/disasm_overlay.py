#!/usr/bin/env python3
"""Disassemble a Tomba!2 overlay .BIN (the stage overlays loaded by FUN_80052078).

The stage overlays (\\BIN\\{START,DEMO,GAME}.BIN, and the assets OPN/CRD/SOP/A0*.BIN)
are read RAW (uncompressed) from the CD by FUN_8001db8c into a fixed RAM base and the
task is restarted at PTR_LAB_800a3ecc[stage]. So file offset X maps to address BASE+X.

The three STAGE overlays all load to the SAME base 0x80106228 (verified: extracted
START.BIN is byte-identical to the resident image at 0x80106228 in ram_f1000.bin), which
is why they can't be disassembled from a single RAM snapshot — only one is resident at a
time. Extract each with:  scratch/bin/fmv_compare dumplba <lba> <size> <out>
  START.BIN LBA 1904 (1648 B)  -> stage 0 entry 0x8010649c  (intro/boot sequencer)
  DEMO.BIN  LBA 1879 (5372 B)  -> stage 1 entry 0x801062e4  (title/attract sequencer)
  GAME.BIN  LBA 1882 (11636 B) -> stage 2 entry 0x8010637c  (gameplay sequencer)

Usage: disasm_overlay.py <overlay.bin> [start_hex] [end_hex] [--base=0x80106228]
Default range = whole file from BASE. Data/string regions disassemble as garbage; that
is expected (overlays interleave code + jump tables + filename strings).
"""
import sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    base = 0x80106228
    for a in sys.argv[1:]:
        if a.startswith("--base="):
            base = int(a.split("=", 1)[1], 16)
    if not args:
        print(__doc__)
        return 1
    path = args[0]
    data = open(path, "rb").read()
    start = int(args[1], 16) if len(args) > 1 else base
    end = int(args[2], 16) if len(args) > 2 else base + len(data)
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    off, n = start - base, end - start
    # Resync past data/string regions: on an undecodable word, emit it as a .word and
    # advance 4 bytes, so the dump covers the whole range instead of stopping at the
    # first jump table / filename string.
    addr = start
    while addr < end:
        chunk = data[addr - base:end - base]
        progressed = False
        for insn in md.disasm(chunk, addr):
            print(f"  {insn.address:08X}  {insn.bytes.hex()}  {insn.mnemonic:8s} {insn.op_str}")
            addr = insn.address + insn.size
            progressed = True
        if not progressed:
            w = data[addr - base:addr - base + 4]
            print(f"  {addr:08X}  {w.hex():8s}  .word    0x{int.from_bytes(w, 'little'):08x}")
            addr += 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
