#!/usr/bin/env python3
"""port_check.py — the equivalence gate: does a native port's guest-visible operation sequence
match its oracle `gen_func_<addr>` body, modulo pure renames?

For every method in a native .cpp carrying a marker comment:
    // PORT_GEN: <addr> <generated-file>:<start>-<end>      (emitted by tools/port_gen.py)
    // ORACLE: gen_func_<addr>  (or ov_<area>_gen_<addr>)     (hand-added convention on an existing
                                                                faithful port — see docs/port-framework.md)
this tool normalizes BOTH the native method body and the `generated/*.c` oracle body to a coarse,
renaming-tolerant "guest-visible operation sequence" (frame descent/ascent sizes, ordered call sites
with their `c->r[31]` return-address constants + resolved target address, ordered memory-store
WIDTHS) via `tools/abi_extract.py`'s `extract_op_sequence` — the SAME parser abi_extract.py uses for
--contract, not a second one — and diffs the two sequences.

Verdict per method:
    PASS        every axis checked (frame open/close sizes, call count+ra-consts+targets, memory-
                 store width sequence) matches exactly.
    FAIL        an axis produced a DEFINITE mismatch (wrong frame size, missing/extra/reordered
                 call, wrong ra constant, wrong call target, wrong memory-store width sequence).
    UNPROVABLE  every axis that COULD be checked matched, but at least one comparison could not be
                 resolved (e.g. an indirect dispatch target that is not a literal on either side, or
                 a native call the tool could not map back to a func_XXXXXXXX/rec_dispatch/
                 guest_call/guest_dispatch site at all). Honesty rule: never silently treat an
                 unresolved comparison as a pass.

Exit code: nonzero if any method FAILs. UNPROVABLE is a warning (does not affect exit code) unless
--strict is given, in which case UNPROVABLE also makes the run fail.

Usage:
    python3 tools/port_check.py <native.cpp> [<native.cpp> ...]
    python3 tools/port_check.py --all                 # scan every tracked .cpp under game/ + runtime/recomp/
    python3 tools/port_check.py <native.cpp> --strict
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi_extract as abi  # the one parser — reused, not forked (see abi_extract.extract_op_sequence)


MARKER_RE = re.compile(
    r'^\s*//\s*(PORT_GEN|ORACLE):\s*(?:0x)?(?:gen_func_|ov_[a-z0-9]+_gen_)?([0-9A-Fa-f]{8})\b'
)
METHOD_DEF_RE = re.compile(r'^[\w:\*&<>,\s]+?\b(\w+)::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{\s*$')

GUEST_CALL_RE = re.compile(r'\bguest_call\(\s*c\s*,\s*0x([0-9A-Fa-f]+)u?\s*,\s*(\w+)\s*\)')
GUEST_DISPATCH_RE = re.compile(r'\bguest_dispatch\(\s*c\s*,\s*0x([0-9A-Fa-f]+)u?\s*,\s*([^)]+)\)')
# Capture BOTH template args: <FrameSize, NumSpills>. FrameSize drives the frame open/close; NumSpills
# is the count of callee-save register spills the ctor performs as `c->mem_w32` stores (guest_abi.h
# GuestFrame ctor: `for i in NumSpills: c->mem_w32(sp+off, r[reg])`) — those N word stores must be
# synthesized on the native side or the store-width sequence under-counts by exactly N vs the oracle
# gen body, whose prologue spills them inline. Every in-tree GuestFrame<> uses two integer literals.
GUESTFRAME_RE = re.compile(r'\bGuestFrame<\s*(\d+)\s*,\s*(\d+)\s*>')


MAX_MARKER_TO_DEF_GAP = 60  # lines — banner comment blocks can be long; cap so a malformed/stray
                             # marker doesn't get mis-attributed to some unrelated, distant method.


def find_method_body(lines: list, start_idx: int):
    """From start_idx (0-based, the marker comment line), scan forward for the next
    `Class::method(...) {` definition line, then return (class, method, body_lines, def_line_no)."""
    i = start_idx
    limit = min(len(lines), start_idx + MAX_MARKER_TO_DEF_GAP)
    while i < limit:
        m = METHOD_DEF_RE.match(lines[i].rstrip('\n'))
        if m:
            cls, method = m.group(1), m.group(2)
            depth = 1
            j = i + 1
            body = []
            in_string = False
            while j < len(lines) and depth > 0:
                line = lines[j]
                k = 0
                clean = []
                while k < len(line):
                    ch = line[k]
                    if ch == '"' and (k == 0 or line[k - 1] != '\\'):
                        in_string = not in_string
                    if not in_string:
                        if ch == '{':
                            depth += 1
                        elif ch == '}':
                            depth -= 1
                            if depth == 0:
                                break
                    clean.append(ch)
                    k += 1
                body.append(''.join(clean))
                j += 1
            return cls, method, body, i
        i += 1
    return None


def find_markers(path: str):
    """Return (lines, hits) where hits is a de-duplicated list of (line_idx, kind, addr) — a
    PORT_GEN and ORACLE marker line for the SAME address within a few lines of each other (both
    emitted for one method, e.g. port_gen.py's banner carries both) collapse to ONE hit so
    find_method_body doesn't stop early on the second marker line while scanning forward."""
    with open(path, 'r', errors='replace') as f:
        lines = f.readlines()
    raw_hits = []
    for i, line in enumerate(lines):
        m = MARKER_RE.match(line.rstrip('\n'))
        if m:
            raw_hits.append((i, m.group(1), m.group(2).upper()))
    hits = []
    for i, kind, addr in raw_hits:
        if hits and hits[-1][2] == addr and i - hits[-1][0] <= 4:
            continue  # same method's second marker line — already counted
        hits.append((i, kind, addr))
    return lines, hits


STRUCT_DEF_RE = re.compile(r'^\s*struct\s+(\w+)\s*\{\s*$')
# Ctor/dtor declaration START lines — the initializer list + opening `{` may spread across several
# following lines (e.g. CmdListFrame's 9-field mem-init list), so these only anchor the FIRST line;
# the actual open-brace line is found by scanning forward for a line ending in `{`.
CTOR_START_RE = re.compile(r'^\s*(?:explicit\s+)?(\w+)\s*\(')
DTOR_START_RE = re.compile(r'^\s*~(\w+)\s*\(\s*\)')
# `constexpr uint32_t FN_8009A450 = 0x8009A450u;` / `static constexpr uint32_t X = 0x...;`
CONST_ADDR_RE = re.compile(r'\bconstexpr\s+uint32_t\s+(\w+)\s*=\s*0x([0-9A-Fa-f]{6,8})u?\s*;')
FRAME_CTOR_CALL_RE = re.compile(r'^\s*(\w*Frame)\s+\w+\(c\)\s*;\s*(?://.*)?$')


def _brace_body(lines: list, open_idx: int):
    """Given the 0-based index of a line ending in `{`, return (body_lines, index_after_close)."""
    depth = 1
    j = open_idx + 1
    body = []
    in_string = False
    while j < len(lines) and depth > 0:
        line = lines[j]
        clean = []
        k = 0
        while k < len(line):
            ch = line[k]
            if ch == '"' and (k == 0 or line[k - 1] != '\\'):
                in_string = not in_string
            if not in_string:
                if ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1
                    if depth == 0:
                        break
            clean.append(ch)
            k += 1
        body.append(''.join(clean))
        j += 1
    return body, j   # j now indexes the line AFTER the closing brace


def find_raii_frame_structs(file_lines: list) -> dict:
    """Scan a whole native .cpp for `struct XxxFrame { ... };` RAII guest-frame idioms (the house
    style — game/render/perobj_dispatch.cpp's CmdListFrame, game/render/cull.cpp's wrapFrame — see
    docs/faithful-execution.md). The struct's constructor/destructor bodies carry the ACTUAL sp
    descent/spill/ascend/restore statements; a checked method only contains a one-line
    `XxxFrame frame(c);` local declaration, so the frame's guest-visible ops are otherwise invisible
    to a per-method body scan. Returns {struct_name: (ctor_body_lines, dtor_body_lines)}."""
    out = {}
    i = 0
    while i < len(file_lines):
        m = STRUCT_DEF_RE.match(file_lines[i].rstrip('\n'))
        if m:
            name = m.group(1)
            struct_body, after = _brace_body(file_lines, i)
            ctor_body, dtor_body = [], []
            j = 0
            while j < len(struct_body):
                line = struct_body[j].rstrip('\n')
                cm = CTOR_START_RE.match(line)
                dm = DTOR_START_RE.match(line)
                is_ctor = cm and cm.group(1) == name and not line.lstrip().startswith('~')
                is_dtor = dm and dm.group(1) == name
                if is_ctor or is_dtor:
                    # find the actual open-brace line (may be several lines below, past a
                    # multi-line member-initializer list)
                    open_j = j
                    while open_j < len(struct_body) and not struct_body[open_j].rstrip().endswith('{'):
                        open_j += 1
                    if open_j < len(struct_body):
                        body, nj = _brace_body(struct_body, open_j)
                        if is_ctor:
                            ctor_body = body
                        else:
                            dtor_body = body
                        j = nj
                        continue
                j += 1
            if ctor_body or dtor_body:
                out[name] = (ctor_body, dtor_body)
            i = after
            continue
        i += 1
    return out


# A bare call-statement the base gen-dialect regexes (CALL_RE/RECDISP_RE, which require the
# func_XXXXXXXX / rec_dispatch spelling) don't recognize — e.g. a sibling-method tail-call like
# `perModeDispatch();` standing in for gen's `func_8003F698(c);`. Recognized ONLY when a
# `c->r[31] = 0x...u;` was just set (same "a call is coming" signal RA_SET_RE anchors) and the
# statement isn't already one of the other recognized call forms — this makes it a call whose
# TARGET is opaque to static analysis (an UNPROVABLE finding, never silently dropped/miscounted).
OPAQUE_CALL_RE = re.compile(r'^\s*[\w:.>-]+\(\s*[^=;]*\)\s*;\s*(?://.*)?$')
KNOWN_NONCALL_KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'else'}


def native_op_sequence_extra(body_lines, frame_structs: dict = None, consts: dict = None):
    """Native-dialect op-sequence extraction. Reuses abi_extract's low-level regexes (RA_SET_RE,
    CALL_RE, RECDISP_RE, MEM_W_ANY_RE, frame regexes via abi.extract_op_sequence) as the shared
    token layer, and layers native-only idioms on top in ONE linear pass so call order stays
    correct relative to the gen-dialect calls the base pass finds: guest_call/guest_dispatch
    (guest_abi.h), GuestFrame<N,...> RAII, XxxFrame struct RAII (spliced from frame_structs), and
    opaque sibling-method calls (target unresolved, but counted — never silently dropped)."""
    if frame_structs:
        expanded = []
        dtor_tail = []
        for raw in body_lines:
            fm = FRAME_CTOR_CALL_RE.match(raw.rstrip('\n'))
            if fm and fm.group(1) in frame_structs:
                ctor_body, dtor_body = frame_structs[fm.group(1)]
                expanded.extend(ctor_body)   # splice ctor ops in place of the one-line declaration
                dtor_tail.extend(dtor_body)  # dtor ops run at scope exit — approximated as tail
                continue
            expanded.append(raw)
        expanded.extend(dtor_tail)
        body_lines = expanded

    # `uint32_t sp0 = c->r[29];` ... `c->r[29] = sp0 - N;` (descent) ... `c->r[29] = sp0;` (ascent,
    # restoring the ALIAS with no literal at all) is a second common native idiom (frameTick,
    # channelReleaseClear, seqChannelDispatch) alongside the `-=N`/`+=N` and gen's raw
    # `c->r[29]+(uint32_t)-N` spellings already handled above. Track sp-aliases so the ascent (which
    # carries no size literal of its own) can be paired with the descent that used the same alias.
    sp_alias_open_size = {}   # alias name -> frame size opened through it, most recent value
    sp_alias_names = set()
    SP_ALIAS_DEF_RE = re.compile(r'^\s*(?:uint32_t\s+)?(\w+)\s*=\s*c->r\[29\]\s*;\s*$')
    SP_ALIAS_OPEN_RE = re.compile(r'^\s*c->r\[29\]\s*=\s*(\w+)\s*-\s*(?:\(?uint32_t\)?)?(\d+)u?\s*;\s*$')
    SP_ALIAS_CLOSE_RE = re.compile(r'^\s*c->r\[29\]\s*=\s*(\w+)\s*;\s*$')

    # Split each source line into ';'-terminated sub-statements — native code (unlike gen's mostly
    # one-statement-per-line emission) routinely puts several on one line
    # (`uint32_t sp = c->r[29]; c->r[29] -= 24;`), and several of the regexes above are anchored to
    # a full statement (`^...;$`) so they need one sub-statement at a time, not the whole raw line.
    substmts = []
    for raw in body_lines:
        for piece in raw.split(';'):
            piece = piece.strip()
            if piece:
                substmts.append(piece + ';')

    frame_opens, frame_closes, widths, calls = [], [], [], []
    pending_ra = None
    for stmt in substmts:
        for gf in GUESTFRAME_RE.finditer(stmt):
            n = int(gf.group(1))
            num_spills = int(gf.group(2))
            frame_opens.append(n)
            frame_closes.append(n)
            # The ctor spills NumSpills callee-save registers as word (32-bit) stores right after the
            # sp descent (guest_abi.h GuestFrame ctor), matching the oracle gen prologue's inline
            # `sw ra/s0.../` spills. Synthesize them here at the declaration point (the prologue), so
            # the native store-width sequence includes them — otherwise it under-counts by NumSpills.
            widths.extend([32] * num_spills)
        # Accept both native's `-=`/`+=` idiom and the raw gen dialect's
        # `c->r[29] = c->r[29] + (uint32_t)-N;` (abi_extract's FRAME_CLOSE_RE additionally requires a
        # trailing `return;` on the SAME statement, which a port_gen-emitted method has verbatim but
        # a split sub-statement here does not — so also accept the bare assignment form directly).
        fo = (abi.FRAME_OPEN_RE.match(stmt) or re.search(r'c->r\[29\]\s*-=\s*(\d+)', stmt) or
              re.search(r'c->r\[29\]\s*=\s*c->r\[29\]\s*\+\s*\(uint32_t\)-(\d+)', stmt))
        fc = (re.search(r'c->r\[29\]\s*\+=\s*(\d+)', stmt) or abi.FRAME_CLOSE_RE.match(stmt) or
              re.search(r'c->r\[29\]\s*=\s*c->r\[29\]\s*\+\s*\(uint32_t\)(\d+)', stmt)) if not fo else None
        if fo:
            size = int(fo.group(1))
            frame_opens.append(size)
            for alias in sp_alias_names:      # `uint32_t sp = c->r[29];` then `c->r[29] -= N;` —
                sp_alias_open_size[alias] = size  # associate whichever alias precedes this descent
        if fc:
            frame_closes.append(int(fc.group(1)))

        sad = SP_ALIAS_DEF_RE.match(stmt)
        if sad:
            sp_alias_names.add(sad.group(1))
        sao = SP_ALIAS_OPEN_RE.match(stmt)
        sac = SP_ALIAS_CLOSE_RE.match(stmt) if not sao else None
        if sao and sao.group(1) in sp_alias_names:
            size = int(sao.group(2))
            frame_opens.append(size)
            sp_alias_open_size[sao.group(1)] = size
        elif sac and sac.group(1) in sp_alias_open_size:
            frame_closes.append(sp_alias_open_size[sac.group(1)])

        for w in abi.MEM_W_ANY_RE.finditer(stmt):
            widths.append(int(w.group(1)))

        gm = GUEST_CALL_RE.search(stmt)
        gd = GUEST_DISPATCH_RE.search(stmt)
        ram = abi.RA_SET_RE.search(stmt)
        cm = abi.CALL_RE.search(stmt)
        rm = abi.RECDISP_RE.search(stmt) if not cm else None

        if gm:
            calls.append(abi.OpSeqCall(ra_const=gm.group(1).upper(), target=gm.group(2)))
            continue
        if gd:
            target_expr = gd.group(2).strip()
            lit = re.match(r'^0x([0-9A-Fa-f]+)u?$', target_expr)
            calls.append(abi.OpSeqCall(ra_const=gd.group(1).upper(),
                                        target=('0x' + lit.group(1).upper()) if lit else None))
            continue
        if ram:
            pending_ra = ram.group(1)
            continue
        if cm:
            calls.append(abi.OpSeqCall(ra_const=pending_ra, target=cm.group(1)))
            pending_ra = None
            continue
        if rm and 'default:' not in stmt:
            raw_target = rm.group(1).strip()
            lit = re.match(r'^0x([0-9A-Fa-f]+)u?$', raw_target)
            if lit:
                target = '0x' + lit.group(1).upper()
            else:
                # A faithful port routinely dispatches through a NAMED constant
                # (`constexpr uint32_t FN_8009A450 = 0x8009A450u; ... rec_dispatch(c, FN_8009A450)`),
                # which is better style than a bare literal — so resolve those instead of punishing
                # them with an unresolvable target. Anything still unknown stays None (honest).
                target = (consts or {}).get(raw_target)
            calls.append(abi.OpSeqCall(ra_const=pending_ra, target=target))
            pending_ra = None
            continue
        if pending_ra is not None and OPAQUE_CALL_RE.match(stmt):
            head = re.match(r'^\s*([\w:.>-]+)\s*\(', stmt)
            word = head.group(1).split('.')[-1].split(':')[-1].split('>')[-1] if head else ''
            # A guest MEMORY primitive (c->mem_w8/16/32, c->mem_r*) is NEVER a sub-function call — but
            # a non-sp-relative store (e.g. c->mem_w16((c->r[16]+50), ...)) isn't consumed by the width
            # matcher above, so it reaches this opaque-call fallback and, sitting between a `c->r[31]=..`
            # ra-const and the real func_X(c) call on the next statement, was mis-counted as a phantom
            # target=None call. This made port_check FALSE-FAIL verbatim port_gen output (24 real calls
            # read as 26) — a workflow defect since port_gen/port_check are one framework. Skip mem_*.
            if word and word not in KNOWN_NONCALL_KEYWORDS and not word.startswith('mem_'):
                calls.append(abi.OpSeqCall(ra_const=pending_ra, target=None))  # opaque target
                pending_ra = None

    return abi.OpSequence(frame_opens=frame_opens, frame_closes=frame_closes,
                           calls=calls, mem_write_widths=widths)


def target_addr(t):
    if t is None:
        return None
    m = re.search(r'([0-9A-Fa-f]{8})$', t)
    return m.group(1).upper() if m else None


def compare(gen_seq: "abi.OpSequence", native_seq: "abi.OpSequence"):
    """Returns (verdict, findings: list[str], unprovable: list[str])."""
    findings = []
    unprovable = []

    # --- frame axis ---
    gen_open = sorted(gen_seq.frame_opens)
    nat_open = sorted(native_seq.frame_opens)
    gen_close = sorted(gen_seq.frame_closes)
    nat_close = sorted(native_seq.frame_closes)
    if gen_open != nat_open:
        findings.append(f"frame OPEN size mismatch: oracle={gen_open} native={nat_open}")
    if gen_close != nat_close:
        findings.append(f"frame CLOSE size mismatch: oracle={gen_close} native={nat_close}")

    # --- call axis ---
    if len(gen_seq.calls) != len(native_seq.calls):
        findings.append(f"call count mismatch: oracle={len(gen_seq.calls)} native={len(native_seq.calls)} "
                         f"(oracle targets={[c.target for c in gen_seq.calls]}, "
                         f"native targets={[c.target for c in native_seq.calls]})")
    else:
        for i, (gc, nc) in enumerate(zip(gen_seq.calls, native_seq.calls)):
            if gc.ra_const is None or nc.ra_const is None:
                unprovable.append(f"call[{i}]: ra constant not resolvable on "
                                   f"{'oracle' if gc.ra_const is None else 'native'} side "
                                   f"(oracle_ra={gc.ra_const} native_ra={nc.ra_const})")
            elif gc.ra_const.upper().lstrip('0') != nc.ra_const.upper().lstrip('0'):
                findings.append(f"call[{i}]: ra constant mismatch: oracle=0x{gc.ra_const} "
                                 f"native=0x{nc.ra_const}")
            ga, na = target_addr(gc.target), target_addr(nc.target)
            if ga is None or na is None:
                unprovable.append(f"call[{i}]: target address not resolvable on "
                                   f"{'oracle' if ga is None else 'native'} side "
                                   f"(oracle_target={gc.target} native_target={nc.target}) — likely an "
                                   f"indirect/jump-table dispatch; verify by hand")
            elif ga != na:
                findings.append(f"call[{i}]: target mismatch: oracle=0x{ga} native=0x{na}")

    # --- memory-store width sequence axis ---
    if gen_seq.mem_write_widths != native_seq.mem_write_widths:
        # find first divergence index for a precise report
        gi, ni = gen_seq.mem_write_widths, native_seq.mem_write_widths
        idx = next((k for k in range(min(len(gi), len(ni))) if gi[k] != ni[k]), min(len(gi), len(ni)))
        findings.append(f"memory-store width sequence mismatch at store #{idx}: "
                         f"oracle={gi} native={ni}")

    if findings:
        return "FAIL", findings, unprovable
    if unprovable:
        return "UNPROVABLE", findings, unprovable
    return "PASS", findings, unprovable


def check_file(path: str, repo: str, results: list):
    lines, hits = find_markers(path)
    if not hits:
        return
    frame_structs = find_raii_frame_structs(lines)
    # File-scope guest-address constants, so `rec_dispatch(c, FN_8009A450)` resolves like a literal.
    consts = {m.group(1): '0x' + m.group(2).upper()
              for m in CONST_ADDR_RE.finditer('\n'.join(lines))}
    for idx, kind, addr in hits:
        found = find_method_body(lines, idx)
        if found is None:
            results.append((path, addr, None, None, "FAIL",
                             [f"marker at line {idx+1} ({kind}: 0x{addr}) has no following "
                              f"Class::method(...) {{ definition — malformed or tool gap"], []))
            continue
        cls, method, body, def_line = found
        try:
            fn = abi.locate_function(repo, addr)
        except abi.AbiParseError as e:
            results.append((path, addr, cls, method, "FAIL",
                             [f"oracle lookup failed: {e}"], []))
            continue
        gen_seq = abi.extract_op_sequence(fn.body)
        native_seq = native_op_sequence_extra(body, frame_structs, consts)
        verdict, findings, unprovable = compare(gen_seq, native_seq)
        results.append((path, addr, cls, method, verdict, findings, unprovable))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='native .cpp files to check')
    ap.add_argument('--all', action='store_true', help='scan every .cpp under game/ and runtime/recomp/')
    ap.add_argument('--strict', action='store_true', help='UNPROVABLE also fails the run')
    ap.add_argument('--repo', default=None)
    args = ap.parse_args(argv)

    # Resolve the recomp corpus root. Order: --repo > $PSXPORT_GAME_ROOT > cwd (if it has generated/) >
    # this framework tool's own repo. Lets the framework porting tools run straight from a consuming game
    # repo (e.g. Tomba2Engine), whose generated/ is game-side after the framework/game repo split.
    repo = (args.repo or os.environ.get('PSXPORT_GAME_ROOT')
            or (os.getcwd() if os.path.isdir(os.path.join(os.getcwd(), 'generated')) else None)
            or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    files = list(args.files)
    if args.all:
        files += sorted(glob.glob(os.path.join(repo, 'game', '**', '*.cpp'), recursive=True))
        files += sorted(glob.glob(os.path.join(repo, 'runtime', 'recomp', '*.cpp')))

    if not files:
        print("port_check: no files given (pass paths or --all)", file=sys.stderr)
        sys.exit(2)

    results = []
    for f in files:
        p = f if os.path.isabs(f) else os.path.join(repo, f)
        if not os.path.isfile(p):
            print(f"port_check: WARNING — {f} not found, skipping", file=sys.stderr)
            continue
        check_file(p, repo, results)

    if not results:
        print("port_check: no // PORT_GEN: / // ORACLE: markers found in the given file(s) — nothing to check")
        sys.exit(0)

    n_pass = n_fail = n_unprov = 0
    for path, addr, cls, method, verdict, findings, unprovable in results:
        rel = os.path.relpath(path, repo)
        label = f"{cls}::{method}" if cls else "?"
        print(f"[{verdict}] {rel} :: {label}  (oracle 0x{addr})")
        for f_ in findings:
            print(f"    FAIL: {f_}")
        for u_ in unprovable:
            print(f"    UNPROVABLE: {u_}")
        if verdict == "PASS":
            n_pass += 1
        elif verdict == "FAIL":
            n_fail += 1
        else:
            n_unprov += 1

    print(f"\nport_check: {n_pass} PASS, {n_fail} FAIL, {n_unprov} UNPROVABLE "
          f"(of {len(results)} methods checked)")

    if n_fail > 0 or (args.strict and n_unprov > 0):
        sys.exit(1)
    sys.exit(0)


if __name__ == '__main__':
    main()
