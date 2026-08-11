#!/usr/bin/env python3
"""exe_similarity.py — ADDRESS-INDEPENDENT code similarity between PS-EXE executables.

THE QUESTION IT ANSWERS: do two PSX games share an ENGINE, or merely the Sony SDK? Every PSX title
links libgte/libgpu/libspu, so raw similarity is never zero and a bare percentage means nothing. What
carries signal is the DELTA between a within-family pair and a cross-family pair, so this tool is only
usable as a MATRIX over several executables — never on one pair in isolation.

METHOD. Decode every aligned 32-bit word of the text region as MIPS, normalise it to an opcode-shape
token (immediates and jump targets MASKED, so the link address cannot matter), hash 16-token shingles,
and report the intersection as a percentage of the smaller shingle set. Identical compiled functions at
different link addresses therefore produce identical shingles.

VALIDATED IN BOTH DIRECTIONS (2026-08-11), which is the only reason its numbers are quotable — a
discriminator run against one class is not known to discriminate:
  * POSITIVE control, Spyro 2 vs Spyro 3 (same studio iterating one codebase): 64.2%
  * NEGATIVE control, Spyro 2 vs Crash 1 (different studios, different engines):  2.3%
  * SDK CEILING, the highest cross-studio pair measured (Spyro 1 vs Crash 2):    12.5%
So >~60% means one codebase; ~10-13% means "shares the SDK and nothing else"; and the ceiling is what
any claim of family membership must clear. Full matrix and conclusions:
`docs/plans/` engine-lineage findings + the workspace decision that cites them.

THE RESULT THAT JUSTIFIES OWNING THIS TOOL: Spyro 1 vs Spyro 2/3 measures 10-11%, i.e. AT the SDK
ceiling. Spyro 1 does NOT share an engine with Spyro 2/3 — Insomniac rewrote it — which no amount of
"they're all Spyro" intuition would have revealed, and which changes how a multi-title port is
structured.

KNOWN LIMITS, stated because a percentage invites over-reading:
  * It compares BOOT EXECUTABLES only. Overlays are not compared, so a game whose engine lives mostly
    in overlays is under-measured.
  * Shingle sharing is necessary-but-not-sufficient evidence that ONE native C++ class can serve two
    titles: struct-layout and behaviour drift only show up during port work. Treat a high score as a
    strong prior for extracting shared code LATER, never as advance justification for designing a
    shared library UP FRONT.
  * The SPECIAL-opcode normalisation drops `rs` as well as masking immediates. That is applied
    uniformly to every input, so comparisons stay valid; it just makes the absolute numbers slightly
    more permissive than the docstring's "regs kept" would imply.

REFUSALS (exit 2, never a clean empty answer): a missing/unreadable file, a file without the PS-X EXE
magic, or an input yielding under 1000 shingles (too small to compare — reported as "scanned almost
nothing", not as 0% similarity).

USAGE
    python3 tools/exe_similarity.py NAME=path/to/SCUS_942.28 NAME2=path/to/SCUS_944.25 ...
    python3 tools/exe_similarity.py --dir <d>      # every PS-EXE under <d>, named by its parent dir

Extract the executables first with the framework's own disc tool (NOT a game's run.sh, which is the
user's play launcher):
    cmake --build build --target discdump
    ./build/tools/discdump list "/path/disc.chd"          # find the boot exe name in SYSTEM.CNF
    ./build/tools/discdump get SCUS_942.28 "/path/disc.chd" <outdir>
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

WINDOW = 16
MIN_SHINGLES = 1000


def norm(w: int) -> int:
    """Normalise one instruction word to an opcode SHAPE, dropping address-dependent fields."""
    op = (w >> 26) & 0x3F
    if op == 0:                     # SPECIAL — no immediate; funct/shamt kept, rs dropped (see limits)
        return w & 0xFC1FFFFF
    if op in (2, 3):                # j / jal — target is a link address, mask it entirely
        return op << 26
    return w & 0xFFFF0000           # I-type — keep op/rs/rt, mask the immediate


def shingles(path: str) -> tuple:
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        raise SystemExit(f"REFUSING: cannot read {path}: {e} — scanned NOTHING.")
    if data[:8] != b'PS-X EXE':
        raise SystemExit(f"REFUSING: {path} has no 'PS-X EXE' magic, so it is not a PSX executable. "
                         f"Nothing was compared; this is not a 0% result.")
    text = data[2048:]
    toks = [norm(struct.unpack_from('<I', text, i)[0]) for i in range(0, len(text) - 4, 4)]
    s = set()
    for i in range(len(toks) - WINDOW):
        s.add(hashlib.blake2b(struct.pack('<%dI' % WINDOW, *toks[i:i + WINDOW]),
                              digest_size=8).digest())
    if len(s) < MIN_SHINGLES:
        raise SystemExit(f"REFUSING: {path} yielded only {len(s)} shingles from {len(toks)} words — too "
                         f"little to compare. That is 'scanned almost nothing', not 'no similarity'.")
    return s, len(toks)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pairs', nargs='*', help='NAME=path entries')
    ap.add_argument('--dir', help='scan this directory for PS-EXEs, naming each by its parent dir')
    args = ap.parse_args()

    items = []
    for p in args.pairs:
        if '=' not in p:
            raise SystemExit(f"expected NAME=path, got {p!r}")
        n, path = p.split('=', 1)
        items.append((n, path))
    if args.dir:
        for root, _dirs, files in os.walk(args.dir):
            for fn in sorted(files):
                fp = os.path.join(root, fn)
                try:
                    with open(fp, 'rb') as f:
                        if f.read(8) != b'PS-X EXE':
                            continue
                except OSError:
                    continue
                items.append((os.path.basename(root) or fn, fp))
    if len(items) < 2:
        print(f"need at least TWO executables to compare — got {len(items)}. A single similarity number "
              f"is meaningless here: only the delta between within-family and cross-family pairs carries "
              f"signal, so nothing was computed.", file=sys.stderr)
        return 2

    sets = {}
    for name, path in items:
        s, nt = shingles(path)
        sets[name] = s
        print(f"{name:10s} {os.path.basename(path):14s} {nt:8d} words -> {len(s)} unique shingles")

    names = [n for n, _ in items]
    print(f"\nintersection as % of the SMALLER shingle set "
          f"(validated scale: >~60% = one codebase, ~10-13% = SDK only):")
    print('%-10s' % '', ''.join('%-8s' % n for n in names))
    for a in names:
        row = '%-10s' % a
        for b in names:
            if a == b:
                row += '%-8s' % '.'
                continue
            inter = len(sets[a] & sets[b])
            row += '%-8s' % ('%.1f%%' % (100.0 * inter / min(len(sets[a]), len(sets[b]))))
        print(row)
    print("\nRead the DELTA, never a single cell: every PSX title links the same Sony SDK, so no pair "
          "reads 0%. Boot executables only — overlays are not compared.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
