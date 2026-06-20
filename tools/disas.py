#!/usr/bin/env python3
"""disas.py — MIPS-I disassembler for Tomba!2's MAIN.EXE (and the BIN overlays), built for the
PC-native engine port. The whole top-down reimplementation needs, per engine function, the EXACT
memory effects: which absolute addresses it reads/writes and at what WIDTH (sb/sh/sw) — guessing a
width silently breaks the interface state the retained PSX content reads back (later-158/159). Ghidra's
`DAT_*` decomp hides widths; the recompiler only covers part of the binary. So this resolves `lui+
addiu/ori` address-builds and annotates every load/store with its resolved target + width.

Usage:
  tools/disas.py <addr> [count]        disassemble from <addr> (hex) until `jr ra` (or count instrs)
  tools/disas.py <addr> --mem          only the loads/stores, with resolved absolute target + width
  tools/disas.py <addr> --raw          no address resolution / annotation
  tools/disas.py --exe <file> ...      use a different PSEXE (default scratch/bin/tomba2/MAIN.EXE)

Resolution is intraprocedural and immediate-only (tracks regs built by lui/ori/addiu/lw-of-const-base);
a target shows as `?` when the base isn't an immediate (e.g. a struct pointer passed in a0). That's
expected — it still tells you the width and the base register.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "recomp"))
import psexe

REG = ["zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
       "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"]
LOADS  = {0x20:"lb",0x21:"lh",0x22:"lwl",0x23:"lw",0x24:"lbu",0x25:"lhu",0x26:"lwr",0x32:"lwc2"}
STORES = {0x28:"sb",0x29:"sh",0x2a:"swl",0x2b:"sw",0x2e:"swr",0x3a:"swc2"}
ALU_I  = {0x08:"addi",0x09:"addiu",0x0a:"slti",0x0b:"sltiu",0x0c:"andi",0x0d:"ori",0x0e:"xori"}
BR_I   = {0x04:"beq",0x05:"bne",0x06:"blez",0x07:"bgtz"}
SPECIAL= {0x00:"sll",0x02:"srl",0x03:"sra",0x04:"sllv",0x06:"srlv",0x07:"srav",0x08:"jr",0x09:"jalr",
          0x0c:"syscall",0x0d:"break",0x10:"mfhi",0x11:"mthi",0x12:"mflo",0x13:"mtlo",
          0x18:"mult",0x19:"multu",0x1a:"div",0x1b:"divu",0x20:"add",0x21:"addu",0x22:"sub",
          0x23:"subu",0x24:"and",0x25:"or",0x26:"xor",0x27:"nor",0x2a:"slt",0x2b:"sltu"}
REGIMM = {0x00:"bltz",0x01:"bgez",0x10:"bltzal",0x11:"bgezal"}

def s16(x): return x - 0x10000 if x & 0x8000 else x

def fmt(w, a, regval):
    """Return (text, target_addr_or_None, width_or_None). `regval` tracks immediate reg values."""
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
    sh = (w >> 6) & 31; imm = w & 0xffff; fn = w & 0x3f
    R = lambda i: REG[i]
    if w == 0: return ("nop", None, None)
    if op == 0:
        if fn in (0x08,):  return (f"jr {R(rs)}", None, None)
        if fn in (0x09,):  return (f"jalr {R(rd)}, {R(rs)}" if rd != 31 else f"jalr {R(rs)}", None, None)
        if fn in (0x0c,0x0d): return (SPECIAL[fn], None, None)
        if fn in (0x10,0x12): return (f"{SPECIAL[fn]} {R(rd)}", None, None)         # mfhi/mflo
        if fn in (0x11,0x13): return (f"{SPECIAL[fn]} {R(rs)}", None, None)         # mthi/mtlo
        if fn in (0x18,0x19,0x1a,0x1b): return (f"{SPECIAL[fn]} {R(rs)}, {R(rt)}", None, None)
        if fn in (0x00,0x02,0x03): return (f"{SPECIAL[fn]} {R(rd)}, {R(rt)}, {sh}", None, None)
        if fn in (0x04,0x06,0x07): return (f"{SPECIAL[fn]} {R(rd)}, {R(rt)}, {R(rs)}", None, None)
        return (f"{SPECIAL.get(fn,'.word')} {R(rd)}, {R(rs)}, {R(rt)}", None, None)
    if op == 0x01: return (f"{REGIMM.get(rt,'regimm?')} {R(rs)}, 0x{a + 4 + (s16(imm)<<2):08x}", None, None)
    if op == 0x02: return (f"j 0x{((a+4)&0xf0000000)|((w&0x3ffffff)<<2):08x}", None, None)
    if op == 0x03: return (f"jal 0x{((a+4)&0xf0000000)|((w&0x3ffffff)<<2):08x}", None, None)
    if op == 0x0f: return (f"lui {R(rt)}, 0x{imm:04x}", None, None)
    if op in BR_I:
        tgt = a + 4 + (s16(imm) << 2)
        if op in (0x06,0x07): return (f"{BR_I[op]} {R(rs)}, 0x{tgt:08x}", None, None)
        return (f"{BR_I[op]} {R(rs)}, {R(rt)}, 0x{tgt:08x}", None, None)
    if op in ALU_I:
        return (f"{ALU_I[op]} {R(rt)}, {R(rs)}, {s16(imm) if op in (0x08,0x09,0x0a,0x0b) else f'0x{imm:04x}'}", None, None)
    if op in LOADS or op in STORES:
        nm = LOADS.get(op) or STORES[op]
        base = regval[rs]
        tgt = (base + s16(imm)) & 0xffffffff if base is not None else None
        width = {0x20:1,0x24:1,0x28:1, 0x21:2,0x25:2,0x29:2, 0x23:4,0x2b:4,0x32:4,0x3a:4}.get(op)
        return (f"{nm} {R(rt)}, {s16(imm)}({R(rs)})", tgt, width)
    if op == 0x10: return (f"cop0 0x{w&0x1ffffff:07x}", None, None)
    if op == 0x12: return (f"cop2/GTE 0x{w&0x1ffffff:07x}", None, None)
    return (f".word 0x{w:08x}", None, None)

def track(w, regval):
    """Update the immediate-value register tracker after instruction w."""
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; imm = w & 0xffff; fn = w & 0x3f
    if op == 0x0f: regval[rt] = (imm << 16) & 0xffffffff               # lui
    elif op == 0x0d and regval[rs] is not None: regval[rt] = regval[rs] | imm        # ori
    elif op == 0x09 and regval[rs] is not None: regval[rt] = (regval[rs] + s16(imm)) & 0xffffffff  # addiu
    elif op == 0:
        if fn in (0x21,0x25) and regval[rs] is not None and rt == 0: regval[rd] = regval[rs]  # addu/or rd,rs,zero (move)
        elif rd != 0: regval[rd] = None
    else:
        # any other op that writes rt invalidates its tracked value
        if op in (0x08,0x0a,0x0b,0x0c,0x0e) : regval[rt] = None
        elif op in LOADS: regval[rt] = None
    regval[0] = 0
    return regval

def main():
    args = sys.argv[1:]
    exe_path = os.path.join(os.path.dirname(__file__), "..", "scratch", "bin", "tomba2", "MAIN.EXE")
    mem_only = raw = False; pos = []
    i = 0
    while i < len(args):
        if args[i] == "--exe": exe_path = args[i+1]; i += 2; continue
        if args[i] == "--mem": mem_only = True; i += 1; continue
        if args[i] == "--raw": raw = True; i += 1; continue
        pos.append(args[i]); i += 1
    if not pos:
        print(__doc__); return 1
    start = int(pos[0], 16)
    count = int(pos[1]) if len(pos) > 1 else 4096
    exe = psexe.load(exe_path)
    regval = [None] * 32; regval[0] = 0
    a = start
    print(f";; {exe_path}  0x{start:08x}")
    for _ in range(count):
        w = exe.word(a)
        text, tgt, width = fmt(w, a, regval)
        ann = ""
        if not raw and tgt is not None:
            ann = f"   ; -> 0x{tgt:08x} ({width}b)" if width else f"   ; -> 0x{tgt:08x}"
        if not mem_only:
            print(f"  {a:08x}: {w:08x}  {text}{ann}")
        elif width is not None:
            print(f"  {a:08x}: {text:24s}{ann}")
        track(w, regval)
        if (w >> 26) == 0 and (w & 0x3f) == 0x08:   # jr (usually jr ra) -> function end after delay slot
            d = exe.word(a + 4)
            if not mem_only:
                td, _, _ = fmt(d, a + 4, regval)
                print(f"  {a+4:08x}: {d:08x}  {td}   ; (delay)")
            break
        a += 4
    return 0

if __name__ == "__main__":
    sys.exit(main())
