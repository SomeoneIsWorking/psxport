#!/usr/bin/env python3
"""frame_audit.py — every native override must mirror its guest stack frame.

A ported guest function that descends `sp` and spills callee-saved registers must reproduce that
frame natively (CLAUDE.md, "MIRROR THE GUEST STACK"). Forgetting it is invisible: the code compiles,
the game runs, and no existing gate catches it —

  * port_check only looks at bodies carrying an `// ORACLE:` marker, and most natives have none;
  * SBS cannot see it either. Measured 2026-07-21: deleting a handler's frame and re-running 1800
    frames of SBS produces ZERO divergences, because these frames are popped before the comparison
    happens and the bytes below `sp` are dead scratch by then. (See docs/findings/sbs.md in the
    consuming repo.) Do not mistake a clean SBS run for proof that a frame is present.

So this audit exists because nothing else can do its job. It is static and needs no disc, no build
and no run:

    python3 external/psxport/tools/frame_audit.py            # audit, print misses
    python3 external/psxport/tools/frame_audit.py --quiet    # exit code only, for a hook

For every native wired to a guest address it asks abi_extract for the oracle's frame size, then
checks the native's own body establishes a frame (`GuestFrame<...>`, a RAII `*Frame` struct, or
explicit `c->r[29]` arithmetic). Non-zero exit if any native is missing one.

STATUS: CANDIDATE FINDER, not yet a gate. VERIFY EVERY HIT BEFORE EDITING.
Two independent finder bugs have already produced confident false positives in this exact task —
matching a forward declaration instead of a definition (63 bogus hits, all pointing at one file), and
attributing a thunk's missing frame to the thunk rather than the body it forwards to. Thunk-following
is implemented here and works on the cases tested, but at least one thunk
(`ov_behSpawnToyType5_80127720`, whose body IS framed) is still reported, and that has not been
explained yet. So: treat output as a worklist to check, confirm each against its own `install(0x…)`
site and `abi_extract --contract`, and do not wire this into a pre-commit hook until the residual
false positive is understood.

Two wiring shapes are recognised:
  overrides::install(0xADDR, "name", native, gen[, setter])   — the framework's own registry
  { 0xADDR, native_symbol, "name" }                           — a game's behaviour dispatch table
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ABI_EXTRACT = os.path.join(HERE, "abi_extract.py")

INSTALL_RE = re.compile(r'\binstall\(\s*0x([0-9A-Fa-f]+)u?\s*,\s*"[^"]*"\s*,\s*([\w:]+)\s*,')
TABLE_RE = re.compile(r'\{\s*0x([0-9A-Fa-f]+)u\s*,\s*(\w+)\s*,\s*"')
FRAME_SIZE_RE = re.compile(r'frame_size = (\d+)')
SPILLS_RE = re.compile(r'prologue spills \((\d+)\)')

# What counts as "this body establishes a guest frame".
HAS_FRAME_RE = re.compile(r'GuestFrame\s*<|c->r\[29\]|\b\w*Frame\s+\w+\s*\(c\)')

# A one-line thunk that forwards to the real body: `{ c->r[2] = beh_real(c, c->r[4]); }` or
# `{ eng(c).sub.method(c->r[4]); }`. The frame belongs in the body it calls, not in the thunk, so a
# thunk reported as frameless is a FALSE POSITIVE — the first run of this audit produced two that way.
THUNK_RE = re.compile(r'^\s*(?:c->r\[2\]\s*=\s*)?(?:[\w()>\-\.]*?\.|[\w]+::)?(\w+)\s*\([^;]*\)\s*;\s*$')


def definition_span(source: str, symbol: str):
    """The body text of `symbol`'s DEFINITION, or None.

    Matching the definition and not the forward declaration is the whole trick: an earlier version of
    this audit matched `void beh_x(Core* c);` in the dispatch-table file, so every hit pointed at that
    one file and the frame test ran against the wrong source — 63 confident false positives.
    """
    bare = symbol.split("::")[-1]
    cls = symbol.split("::")[-2] if "::" in symbol else None
    pattern = (r'^[\w:\*&<>,\s]*\b'
               + (re.escape(cls) + r'::' if cls else r'')
               + re.escape(bare) + r'\s*\([^;{]*\)\s*\{')
    m = re.search(pattern, source, re.M)
    if not m:
        return None
    i, depth = m.end(), 1
    while i < len(source) and depth:
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
        i += 1
    return source[m.end():i]


def oracle_frame(addr: str):
    """(frame_size, spill_count) from the recompiled oracle body, or None if it has no contract."""
    try:
        out = subprocess.run([sys.executable, ABI_EXTRACT, "0x" + addr, "--contract"],
                             capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = FRAME_SIZE_RE.search(out)
    if not m:
        return None
    spills = SPILLS_RE.search(out)
    return int(m.group(1)), int(spills.group(1)) if spills else 0


def collect(roots):
    """guest addr -> (native symbol, defining file). Later definitions do not overwrite earlier ones."""
    wired, sources = {}, {}
    for root in roots:
        for path in glob.glob(os.path.join(root, "**", "*.cpp"), recursive=True):
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            sources[path] = text
            for m in INSTALL_RE.finditer(text):
                wired.setdefault(m.group(1).upper(), m.group(2))
            for m in TABLE_RE.finditer(text):
                wired.setdefault(m.group(1).upper(), m.group(2))

    located = {}
    for addr, sym in wired.items():
        for path, text in sources.items():
            body = definition_span(text, sym)
            if body is None:
                continue
            # Follow a one-line thunk to the body it forwards to — that is where the frame belongs.
            stripped = [ln for ln in body.strip().split("\n") if ln.strip()]
            if len(stripped) == 1:
                m = THUNK_RE.match(stripped[0])
                if m:
                    for p2, t2 in sources.items():
                        inner = definition_span(t2, m.group(1))
                        if inner is not None and m.group(1) != sym.split("::")[-1]:
                            sym, path, body = m.group(1), p2, inner
                            break
            located[addr] = (sym, path, body)
            break
    return located


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="*", default=["game", "runtime"],
                    help="directories to scan (default: game runtime)")
    ap.add_argument("--quiet", action="store_true", help="exit code only")
    args = ap.parse_args(argv)

    roots = [r for r in args.roots if os.path.isdir(r)]
    if not roots:
        print("frame_audit: no source roots found — run from the consuming repo root", file=sys.stderr)
        return 2

    located = collect(roots)
    misses, checked, unresolved = [], 0, 0
    for addr, (sym, path, body) in sorted(located.items()):
        contract = oracle_frame(addr)
        if contract is None:
            unresolved += 1
            continue
        size, spills = contract
        if size == 0:
            continue
        checked += 1
        if not HAS_FRAME_RE.search(body):
            misses.append((addr, sym, path, size, spills))

    if not args.quiet:
        print(f"frame_audit: {len(located)} natives wired to a guest address, "
              f"{checked} with a non-zero oracle frame, {unresolved} without a recompiled contract")
        for addr, sym, path, size, spills in misses:
            print(f"  MISSING FRAME  0x{addr}  frame={size:<3} spills={spills:<2} {sym}  ({path})")
        if misses:
            print(f"\n{len(misses)} native(s) do not mirror their guest stack frame.\n"
                  f"Fix each with:  python3 external/psxport/tools/abi_extract.py 0x<ADDR> "
                  f"--scaffold --guestabi\nwhich emits the spill table and the GuestFrame<> line — "
                  f"never hand-derive them.")
        else:
            print("all good — every wired native with a guest frame mirrors it")
    return 1 if misses else 0


if __name__ == "__main__":
    sys.exit(main())
