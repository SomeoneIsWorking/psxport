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

STATUS: UNRELIABLE — DO NOT ACT ON THIS OUTPUT WITHOUT READING THE CODE.

This script has produced FIVE separate waves of confident false positives:
  1. markers bound to the next `Class::method`, so free-function natives reported `registerOverrides`
  2. symbol resolution matching forward declarations instead of definitions (63 hits, one file)
  3. bodies extracted WITH the closing brace, so one-line thunks never matched (43 hits)
  4. trailing `//` comments defeating end-of-line anchors (a caller check inverted, 54/54)
  5. thunk-following missing four sop_intro handlers whose bodies ARE hand-framed at
     sop_intro_events.cpp lines 75 / 137 / 189 / 273

Acting on wave 5 would have added a SECOND frame to correctly-framed functions. Acting on an earlier
list (the Cull wrappers) did produce a real SBS divergence.

The reason is structural, not a run of bad luck: "where is this defined", "is this body a forwarding
thunk", "who calls this" are semantic questions about C++, and this file answers them with line
patterns. Every pattern has an edge case, and the edge case surfaces only after the output looks
authoritative.

USE IT AS A HINT, NEVER AS A VERDICT. For each hit, open the function, read its body and its RE notes,
and check `gen_func_<ADDR>` before touching anything. Rebuilding this on libclang (the repo has
build/compile_commands.json and /usr/lib64/libclang.so) would make the questions exact and retire the
whole false-positive class; until then, reading beats running this.
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
    # i now sits one PAST the closing brace, so drop it: leaving the '}' on the end made a one-line
    # thunk body fail THUNK_RE's `;\s*$` anchor, which is exactly why a framed thunk kept being
    # reported as missing a frame.
    return source[m.end():max(m.end(), i - 1)]


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
            # Follow a thunk to the body it forwards to — that is where the frame belongs. A thunk is
            # ONE delegating call plus, optionally, trivial register bookkeeping like `c->r[2] = 0;`
            # (`eov_propagateRotmat` is exactly that shape). Restricting this to single-line bodies
            # left three whole files reported as frameless when their real bodies were framed.
            # Strip // comments before classifying: a trailing comment on the `c->r[2] = 0;` line, or
            # a standalone comment line, otherwise counts as a second "statement" and stops the body
            # from being recognised as a thunk (this hid buildFromChild / triggerPanned / the sop_*
            # handlers, whose real bodies are framed).
            no_comments = [re.sub(r'//.*$', '', ln).strip() for ln in body.strip().split("\n")]
            stripped = [ln for ln in no_comments if ln]
            bookkeeping = re.compile(r'^c->r\[\d+\]\s*=\s*[\w()\-]+\s*;$')
            calls = [ln for ln in stripped if not bookkeeping.match(ln)]
            if len(calls) == 1:
                m = THUNK_RE.match(calls[0])
                if m:
                    for p2, t2 in sources.items():
                        inner = definition_span(t2, m.group(1))
                        if inner is not None and m.group(1) != sym.split("::")[-1]:
                            sym, path, body = m.group(1), p2, inner
                            break
            # A method may legitimately be UNFRAMED because the frame lives in a sibling `<name>Framed`
            # wrapper: the unframed method is what native callers use, the wrapper is the guest-ABI
            # entry. cull.cpp states the rule outright ("Framing now lives ONLY in
            # cullWrapperFlag2Framed()"). Moving the frame into the method instead produced a REAL SBS
            # divergence at 0x801FE8D4 — measured, then reverted — so this is not a cosmetic detail.
            bare = sym.split("::")[-1]
            if not HAS_FRAME_RE.search(body):
                sibling = definition_span(text, bare + "Framed")
                if sibling is not None and HAS_FRAME_RE.search(sibling):
                    body = sibling
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
