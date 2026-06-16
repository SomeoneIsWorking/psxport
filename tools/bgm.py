#!/usr/bin/env python3
"""Definitive Tomba!2 BGM / libsnd-sequencer state inspector.

Works on any 2MB PSX main-RAM dump — the oracle's reliable scene snapshots
(scratch/bin/tomba2/state/*.bin) AND our native port's dumps (PSXPORT_RAMDUMP).
Stops the BGM hunt from depending on flaky live navigation: dump a known scene
from each core and diff the BGM state directly.

Addresses (KSEG0) — RE'd in journal later-75:
  0x800bed80  current-song byte (idx into the per-song table; 0xFF = no song)
  0x800b6e68  per-song table, stride 8: {+0: seq_no (h), +4: ... }  (FUN_80074BF8 lookup)
  0x80104c30  ptr to the libsnd seq-struct array base (= 0x800be3d8)
  0x801054b0  open-seq count (h)        0x80104c28  playing bitmask
  0x800ac424  tick mode (b)             0x800ac42c  SsSeqCalled fn ptr
  0x800be22a  per-frame BGM request byte (FUN_80075A80 sound-command service)
  seq slot i = base + i*0xB0:  +0x00 read ptr, +0x04 SEQ base, +0x98 flag (bit0 = active/playing)

Usage:
  bgm.py dump   <ramfile> [ramfile2 ...]   # one-line BGM summary per dump (diff-friendly)
  bgm.py slots  <ramfile>                  # full per-slot table
  bgm.py callers <ramfile> <hexaddr>       # jal callers + data-word refs to addr (RE aid)
"""
import sys, struct

def load(path):
    d = open(path, "rb").read()
    if len(d) < 0x200000:
        d = d + b"\0" * (0x200000 - len(d))
    return d

def u32(d, a): return struct.unpack("<I", d[a & 0x1FFFFC:(a & 0x1FFFFC) + 4])[0]
def u16(d, a): return struct.unpack("<H", d[a & 0x1FFFFE:(a & 0x1FFFFE) + 2])[0]
def u8(d, a):  return d[a & 0x1FFFFF]

SLOT_BASE_PTR = 0x80104c30
N_SLOTS = 14
STRIDE = 0xB0

def slot_base(d): return u32(d, SLOT_BASE_PTR)

def active_slots(d):
    base = slot_base(d)
    out = []
    if not base:
        return out
    for i in range(N_SLOTS):
        s = base + i * STRIDE
        flag = u32(d, s + 0x98)
        if flag & 1:
            rd, sb = u32(d, s), u32(d, s + 4)
            out.append((i, rd, sb, rd - sb, flag))
    return out

def summary(path):
    d = load(path)
    base = slot_base(d)
    idx = u8(d, 0x800bed80)
    seq_no = None
    if idx != 0xFF and idx < 0x40:
        seq_no = u16(d, 0x800b6e68 + idx * 8)
    act = active_slots(d)
    act_str = " ".join(f"#{i}(rd{dlt:+d})" for i, rd, sb, dlt, fl in act) or "none"
    print(f"{path}")
    print(f"  song@800bed80={idx:#04x}{'' if idx==0xFF else f' -> seq_no={seq_no}'}"
          f"  req@800be22a={u8(d,0x800be22a):#04x}")
    print(f"  seqtbl@80104c30={base:#010x}  open={u16(d,0x801054b0)}"
          f"  playmask={u16(d,0x80104c28):#06x}  tickmode={u8(d,0x800ac424)}"
          f"  seqfn={u32(d,0x800ac42c):#010x}")
    print(f"  active(bit0) slots: {act_str}")

def slots(path):
    d = load(path)
    base = slot_base(d)
    print(f"{path}  seqtbl={base:#010x} song={u8(d,0x800bed80):#04x}")
    if not base:
        print("  (libsnd not initialised)"); return
    print("  slot  struct      flag      bit0  readptr    SEQbase    rd-base  vL   vR")
    for i in range(N_SLOTS):
        s = base + i * STRIDE
        flag = u32(d, s + 0x98)
        rd, sb = u32(d, s), u32(d, s + 4)
        vl, vr = u16(d, s + 0x52), u16(d, s + 0x54)
        print(f"  {i:>2}    {s:08X}  {flag:08X}  {flag&1}     "
              f"{rd:08X}   {sb:08X}   {rd-sb:+6d}  {vl:04X} {vr:04X}")

def callers(path, addr):
    d = load(path)
    jal = 0x0C000000 | ((addr >> 2) & 0x03FFFFFF)
    def find(pat):
        out, i = [], d.find(pat)
        while i != -1:
            out.append(0x80000000 + i); i = d.find(pat, i + 1)
        return out
    jc = find(struct.pack("<I", jal))
    dr = find(struct.pack("<I", addr))
    print(f"{addr:#010x} in {path}")
    print(f"  jal callers ({len(jc)}): " + " ".join(f"{x:08X}" for x in jc))
    print(f"  data refs   ({len(dr)}): " + " ".join(f"{x:08X}" for x in dr))

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    cmd = sys.argv[1]
    if cmd == "dump":
        for p in sys.argv[2:]:
            summary(p)
    elif cmd == "slots":
        slots(sys.argv[2])
    elif cmd == "callers":
        callers(sys.argv[2], int(sys.argv[3], 16))
    else:
        print(__doc__); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
