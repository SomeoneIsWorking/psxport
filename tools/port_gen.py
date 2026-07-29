#!/usr/bin/env python3
"""port_gen.py — draft-generator for a byte-faithful native class-method, day one.

Purpose (see docs/port-framework.md, CLAUDE.md "MIRROR THE GUEST STACK"): the recurring bug class
in hand-porting a `gen_func_<addr>` body is the agent RE-TYPING the guest-visible operations from
memory/eyeball and getting a stack offset, an `ra` constant, or a callee-saved register wrong. This
tool removes that transcription step entirely: it takes the gen body VERBATIM (same `c->r[]`,
`c->mem_w*`/`c->mem_r*`, `func_XXXXXXXX(c)`/`rec_dispatch(c, ...)` calls, labels/gotos, the exact
`c->r[31] = 0x...u;` constants) and wraps it as a class method. The body is byte-faithful BY
CONSTRUCTION — there is nothing to transcribe wrong on day one. The agent's subsequent job is
RENAMING locals/labels into named struct fields and control flow (verified equivalent by
`tools/port_check.py`), never re-deriving the ABI/stack shape from scratch.

This tool REUSES tools/abi_extract.py's function locator (`locate_function`) — it does not
re-implement a second generated/*.c parser. See docs/abi-extract.md for the parsing dialect notes.

Usage:
    python3 tools/port_gen.py <hexaddr> --class ClassName --method methodName \
        [--file game/subsys/foo.cpp] [--header game/subsys/foo.h] [--core-member mCore|core] \
        [--repo <path>]

If --header does not exist, a minimal header declaring `class ClassName` with the method and a
`Core* <core-member>` pointer is created. If --header already exists, the tool APPENDS nothing to
it automatically (editing an existing class header mechanically is unsafe) — it prints the method
declaration line the agent must add by hand, and still emits the .cpp body.

The emitted .cpp is UNWIRED dead code: nothing calls the new method. It must be added to the cmake
source list and must compile+link before an agent starts renaming. It is NOT registered with any
dispatch table — wiring is a separate, later, deliberate step (see docs/fleet-workflow.md §9).
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi_extract as abi  # reuse the ground-truth generated/*.c locator + parser, don't fork one


def emit_header(class_name: str, method: str, core_member: str) -> str:
    guard = f"{class_name.upper()}_H"
    return (
        f"#pragma once\n"
        f"// {class_name} — PORT_GEN scaffold class (see docs/port-framework.md). UNWIRED.\n"
        f"// Regenerate the body with: python3 tools/port_gen.py <addr> --class {class_name} "
        f"--method {method}\n"
        f"class Core;\n\n"
        f"class {class_name} {{\n"
        f"public:\n"
        f"  Core* {core_member} = nullptr;\n\n"
        f"  void {method}();\n"
        f"}};\n"
    )


def emit_cpp(fn: abi.FoundFunction, class_name: str, method: str, core_member: str,
             header_include: str) -> str:
    rel_gen = os.path.relpath(fn.path, os.path.dirname(os.path.abspath(__file__)) + '/..')
    out = []
    out.append(f"// {os.path.basename(header_include).replace('.h', '.cpp')} — PORT_GEN draft, byte-faithful transcription of {fn.name}.")
    out.append(f"// ORACLE: {fn.name} ({rel_gen}:{fn.start_line}-{fn.end_line})")
    out.append(f"// PORT_GEN: 0x{fn.addr} {rel_gen}:{fn.start_line}-{fn.end_line}")
    out.append("//")
    out.append("// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,")
    out.append("// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is")
    out.append("// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING")
    out.append("// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.")
    out.append("// UNWIRED — dead code. Do not wire into any dispatch table before running port_check.py")
    out.append("// and the mandatory line-by-line verify pass (docs/fleet-workflow.md §9).")
    out.append(f'#include "{header_include}"')
    out.append('#include "core.h"')
    out.append("")
    out.append("void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point")
    # Forward-declare every direct func_XXXXXXXX/ov_<area>_func_XXXXXXXX callee the body invokes —
    # same pattern as game/render/perobj_dispatch.cpp's explicit `void func_800803DC(Core*);` line.
    # LIVE EXTENT. The recompiler FOLDS contiguous guest functions into one gen_func_ body: the real
    # function ends at a bare `return;` and the NEXT function's code follows as unreachable trailing
    # text before the closing brace (abi_extract.py:869-885 documents the artifact and excludes it,
    # which is why it reports a large unreachable_block_count on these).
    #
    # Emitting that tail is not harmless-but-untidy. It puts a DIFFERENT FUNCTION'S BODY inside this
    # method, and it defeats the checks: port_check compares static store sequences and FAILS, while a
    # whole-body diff against the gen text still says "identical" — because a diff against the wrong
    # extent cannot detect a wrong extent. That combination shipped a 74-line foreign tail into
    # Input::setVoiceVolume on 2026-07-29 and then produced a confident, wrong accusation that
    # abi_extract was broken. Cut it here, at the source, so no consumer has to notice.
    live_body, foreign = _split_live_extent(fn.body)

    callees = []
    seen = set()
    for raw in live_body:
        for m in abi.CALL_RE.finditer(raw):
            name = m.group(1)
            if name not in seen:
                seen.add(name)
                callees.append(name)
    for name in callees:
        out.append(f"void {name}(Core*);  // generated/{fn.ov_name + '_' if fn.is_overlay else ''}shard_disp.c")
    out.append("")
    out.append(f"void {class_name}::{method}() {{")
    out.append(f"  Core* c = {core_member};")
    for line in live_body:
        out.append("  " + line if line.strip() else "")
    out.append("}")
    if foreign:
        out.append("")
        out.append(f"// port_gen TRIMMED {len(foreign)} line(s) of FOLDED-SIBLING tail after this")
        out.append(f"// function's real `return;` — they belong to the next guest function, not to")
        out.append(f"// 0x{fn.addr}. abi_extract counts them in unreachable_block_count. Nothing is lost:")
        out.append(f"// the next function has its own address and can be ported on its own.")
    out.append("")
    return "\n".join(out) + "\n"


def _split_live_extent(body):
    """Split a gen body into (live, foreign) at the first bare `return;`.

    Every `return;` a recompiled body emits is unconditional (it is a `jr ra`; conditional flow is
    always a branch/goto), so statements after the first one are reachable only via a label — i.e.
    they belong to a folded sibling function, not to this one. Returns (body, []) when there is no
    trailing text, which is the common case.
    """
    import re as _re
    label_def = _re.compile(r'^([A-Za-z_]\w*):\s*;?\s*$')
    goto_ref = _re.compile(r'goto\s+([A-Za-z_]\w*)\s*;')

    for i, line in enumerate(body):
        if line.strip() != 'return;':
            continue
        head, tail = body[:i + 1], body[i + 1:]
        if not [l for l in tail if l.strip()]:
            return body, []

        # MEASURED over all 2119 main-EXE gen functions (2026-07-29): 1788 clean, 282 with a folded tail
    # that is now trimmed (largest 16,861 foreign lines at 0x8009D06C), and 49 with trailing text
    # where THIS GUARD HELD. That last number is the justification for the guard existing: without
    # it, a naive "trim at the first return;" would have deleted REACHABLE code in 49 functions —
    # 15% of every case with trailing text. The guard is not defensive programming, it is the
    # difference between a sound transform and a silent corruption in one port out of seven.
    #
    # SOUNDNESS GUARD. "Everything after the first return; is a folded sibling" is only true if
        # the live part never jumps INTO that region. A recompiled body can place a legitimate
        # continuation behind an early return and reach it by label — trimming there would delete
        # reachable code and, at best, fail to compile. So only trim when no label defined in the
        # tail is targeted from the head. If one is, this is not a sibling boundary: leave the body
        # whole and let port_check speak.
        tail_labels = {m.group(1) for l in tail for m in [label_def.match(l.strip())] if m}
        head_targets = {m.group(1) for l in head for m in goto_ref.finditer(l)}
        if tail_labels & head_targets:
            return body, []
        return head, tail
    return body, []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('addr', help='guest address, hex, e.g. 8001CE90 or 0x8001CE90')
    ap.add_argument('--class', dest='class_name', required=True, help='target C++ class name')
    ap.add_argument('--method', required=True, help='target method name')
    ap.add_argument('--file', help='output .cpp path (default: derived from --class under game/core/)')
    ap.add_argument('--header', help='output .h path (default: same basename as --file, .h)')
    ap.add_argument('--core-member', default='mCore', help='name of the Core* member (default mCore)')
    ap.add_argument('--repo', default=None)
    args = ap.parse_args(argv)

    repo = args.repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    try:
        fn = abi.locate_function(repo, args.addr)
    except abi.AbiParseError as e:
        print(f"port_gen: FAILED — {e}", file=sys.stderr)
        sys.exit(1)

    cpp_path = args.file or os.path.join('game', 'core', f'{args.class_name.lower()}.cpp')
    header_path = args.header or os.path.splitext(cpp_path)[0] + '.h'
    header_include = os.path.relpath(header_path, os.path.join(repo, 'game'))

    header_abs = os.path.join(repo, header_path)
    cpp_abs = os.path.join(repo, cpp_path)

    header_existed = os.path.isfile(header_abs)
    if not header_existed:
        os.makedirs(os.path.dirname(header_abs), exist_ok=True)
        with open(header_abs, 'w') as f:
            f.write(emit_header(args.class_name, args.method, args.core_member))
        print(f"port_gen: wrote new header {header_path}")
    else:
        print(f"port_gen: {header_path} already exists — NOT modified. Add by hand if missing:\n"
              f"    void {args.method}();   // Core* {args.core_member} member assumed to already exist")

    contract = abi.parse_contract(fn)
    os.makedirs(os.path.dirname(cpp_abs), exist_ok=True)
    with open(cpp_abs, 'w') as f:
        f.write(emit_cpp(fn, args.class_name, args.method, args.core_member, header_include))
    _live, _foreign = _split_live_extent(fn.body)
    if _foreign:
        print(f"port_gen: TRIMMED {len(_foreign)} folded-sibling line(s) after the real `return;` "
              f"— they belong to the NEXT guest function, not 0x{fn.addr}")
    print(f"port_gen: wrote {cpp_path}  ({fn.name}, {len(_live)} live body lines, "
          f"frame_size={contract.frame_size}, {len(contract.call_sites)} call site(s))")
    print(f"port_gen: NEXT — add {cpp_path} to cmake/tomba2_port.cmake, build, then run "
          f"tools/port_check.py {cpp_path}")


if __name__ == '__main__':
    main()
