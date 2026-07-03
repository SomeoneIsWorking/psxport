#!/usr/bin/env python3
# symres.py — resolve guest addresses to symbol names.
#
# WHY: dispwatch / guest_backtrace_to / abort reports print raw hex addresses (`ra=8007E9C8`,
# `[sp+0x018] 0x80051844`). Reading these by eye means grepping code-map / rec_decls each time,
# which is slow and hides call context. This tool resolves them:
#   1. NATIVE  (docs/code-map.md)        → `Class::method` at file:line
#   2. RECOMP  (generated/rec_decls.h)   → `func_XXXXXXXX` (resident MAIN.EXE recompiled body)
#   3. UNKNOWN                           → address kept as-is (overlay code / BIOS / data)
#
# USAGE:
#   tools/symres.py 0x8007E9C8 0x80051844                 # explicit addresses
#   ./scratch/bin/tomba2_port 2>&1 | tools/symres.py -    # annotate a full stderr stream
#   tools/symres.py --grep-map <keyword>                  # find natives by name
#
# The stream mode is the primary form — pipe any `dispwatch` / `[abort]` / `[VSYNC-TRAP]` output
# through it and every recognisable 0x8xxxxxxx becomes labeled inline. That IS the "call stack for
# a fade" the user asked for.
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODEMAP = os.path.join(ROOT, "docs", "code-map.md")
DECLS   = os.path.join(ROOT, "generated", "rec_decls.h")

_natmap = None    # addr(int) -> (status, name, ref)
_recompset = None # set of addr(int) for resident recompiled bodies

def _load_natives():
    """Parse docs/code-map.md rows: `| 0xADDR | STATUS | Class::method | file.cpp:N | notes |`."""
    global _natmap
    if _natmap is not None: return _natmap
    _natmap = {}
    if not os.path.exists(CODEMAP): return _natmap
    with open(CODEMAP) as f:
        for line in f:
            # match rows starting with `| 0x`
            m = re.match(r"^\|\s*0x([0-9A-Fa-f]+)\s*\|\s*(\S+)\s*\|\s*([^|]+?)\s*\|\s*([^|]*?)\s*\|", line)
            if not m: continue
            addr = int(m.group(1), 16)
            _natmap[addr] = (m.group(2), m.group(3).strip("`"), m.group(4))
    return _natmap

def _load_recomp():
    """Parse generated/rec_decls.h for `func_XXXXXXXX` entries — the recompiled-body address set."""
    global _recompset
    if _recompset is not None: return _recompset
    _recompset = set()
    if not os.path.exists(DECLS): return _recompset
    with open(DECLS) as f:
        for line in f:
            m = re.search(r"\bfunc_([0-9A-F]{8})\b", line)
            if m: _recompset.add(int(m.group(1), 16))
    return _recompset

def resolve(addr):
    """Return (label, kind) or (None, 'UNKNOWN') if not recognised."""
    a = addr & 0xFFFFFFFF
    a_masked = (a & 0x1FFFFFFF) | 0x80000000    # KSEG1 -> KSEG0 normalisation
    nat = _load_natives()
    rec = _load_recomp()
    if a in nat: st, nm, ref = nat[a]; return (f"{nm} [{st}] @ {ref}", "NATIVE")
    if a_masked in nat: st, nm, ref = nat[a_masked]; return (f"{nm} [{st}] @ {ref}", "NATIVE")
    if a in rec: return (f"func_{a:08X}", "RECOMP")
    if a_masked in rec: return (f"func_{a_masked:08X}", "RECOMP")
    return (None, "UNKNOWN")

_HEX = re.compile(r"\b0x([0-9A-Fa-f]{6,8})\b|\b([0-9A-Fa-f]{8})\b")

def _annotate_line(line):
    """Inline-annotate every hex address on a line. Skips ones that don't resolve."""
    parts = []
    last = 0
    for m in _HEX.finditer(line):
        parts.append(line[last:m.end()])
        hexs = m.group(1) or m.group(2)
        addr = int(hexs, 16)
        if addr < 0x10000: continue
        label, kind = resolve(addr)
        if label:
            parts.append(f" ⟨{label}⟩")
        last = m.end()
    parts.append(line[last:])
    return "".join(parts)

def _cmd_grep(kw):
    kw = kw.lower()
    for addr, (st, nm, ref) in sorted(_load_natives().items()):
        if kw in nm.lower() or kw in ref.lower():
            print(f"  0x{addr:08X}  [{st:8}]  {nm}  ({ref})")

def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip("\n")); return
    if argv[0] == "--grep-map":
        if len(argv) < 2: sys.exit("usage: symres.py --grep-map <keyword>")
        _cmd_grep(argv[1]); return
    if argv[0] == "-":
        # stream mode: annotate stdin line-by-line
        for line in sys.stdin: sys.stdout.write(_annotate_line(line))
        return
    for s in argv:
        try: addr = int(s, 16 if s.startswith("0x") else 0)
        except ValueError: print(f"  {s}  ⟨not a number⟩"); continue
        label, kind = resolve(addr)
        print(f"  0x{addr:08X}  [{kind:8}]  {label or '(unknown — overlay / BIOS / data)'}")

if __name__ == "__main__":
    main(sys.argv[1:])
