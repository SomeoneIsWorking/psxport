#!/usr/bin/env python3
"""abi_extract.py — static ABI/stack-contract extractor for recompiled gen function bodies.

Purpose (see CLAUDE.md "MIRROR THE GUEST STACK" + docs/faithful-execution.md): the #1 recurring bug
class when hand-porting a `gen_func_<addr>` / `ov_<area>_gen_<addr>` body to a native C++ class method
is getting the guest-stack frame / callee-save spill / r31-before-call / callee-saved-register-liveness
boilerplate wrong. This tool parses the RECOMPILED C TEXT (ground truth — never Ghidra's pseudo-C,
which garbles COP2/delay slots) and emits:

  1. --contract (default): a human-readable + JSON-able description of the function's guest-ABI
     contract: frame size, ordered sp-relative stores (prologue spills / scratch stores / epilogue
     restores) in PROGRAM ORDER, and every call site with its target, its r31 (return-address) constant,
     and which callee-saved registers (r16..r23, r30) look live going into it.
  2. --scaffold: a ready-to-paste C++ Frame RAII struct + call-site comment block in the house style
     (game/render/perobj_dispatch.cpp's CmdListFrame / game/world/object_table.cpp's frame idiom).
  3. --audit <native.cpp>: best-effort heuristic grep-level check of an EXISTING native port against
     the contract (frame delta present? ra constants present? spill offsets present?). This is
     intentionally noisy/heuristic — labelled as such in the output; it is not a substitute for the
     line-by-line RE verify pass docs/fleet-workflow.md §9 requires.

This is a STATIC TEXT parser over the recompiler's fixed emission idioms, not a general MIPS/C dataflow
engine. It recognizes the emission patterns actually seen in generated/*.c; if it hits a function body
whose shape doesn't match those idioms, it FAILS LOUDLY (raises AbiParseError) rather than emit a
guessed/wrong contract — a wrong contract is worse than none (see CLAUDE.md "No bandaids").

Usage:
    python3 tools/abi_extract.py <hexaddr> [--contract|--scaffold|--audit <native.cpp>] [--json] [--repo <path>]

Pure stdlib. No new dependencies.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from dataclasses import dataclass, field, asdict
from typing import Optional


# --------------------------------------------------------------------------------------------------
# Function location
# --------------------------------------------------------------------------------------------------

# Shard bodies:   void gen_func_8003CDD8(Core* c) {
# Overlay bodies: void ov_a00_gen_8013FB88(Core* c) {
FUNC_OPEN_RE = re.compile(
    r'^void ((?:ov_[a-z0-9]+_gen_|gen_func_)([0-9A-Fa-f]{8}))\(Core\* c\) \{\s*$'
)


class AbiParseError(Exception):
    pass


@dataclass
class FoundFunction:
    name: str          # e.g. gen_func_8003CDD8 / ov_a00_gen_8013FB88
    addr: str          # 8-hex-digit, uppercase
    path: str          # source file
    start_line: int    # 1-based, the "void ...(Core* c) {" line
    end_line: int      # 1-based, the matching "}" line
    body: list          # raw lines of the body (EXCLUDING the open/close brace lines), each a str
    is_overlay: bool
    ov_name: Optional[str]  # e.g. "ov_a00" or None for shard


def locate_function(repo: str, hexaddr: str) -> FoundFunction:
    """Scan generated/*.c (shards + overlays) for the gen body at hexaddr."""
    addr = hexaddr.upper().lstrip('0X') if hexaddr.lower().startswith('0x') else hexaddr.upper()
    addr = addr.zfill(8)
    gen_dir = os.path.join(repo, 'generated')
    if not os.path.isdir(gen_dir):
        raise AbiParseError(f"generated/ not found under {repo!r} — copy/build the recomp corpus first")

    candidates = sorted(glob.glob(os.path.join(gen_dir, '*.c')))
    if not candidates:
        raise AbiParseError(f"no *.c files under {gen_dir}")

    for path in candidates:
        with open(path, 'r', errors='replace') as f:
            lines = f.readlines()
        for i, line in enumerate(lines):
            m = FUNC_OPEN_RE.match(line.rstrip('\n'))
            if not m:
                continue
            name, faddr = m.group(1), m.group(2).upper()
            if faddr != addr:
                continue
            # find the matching close: recompiler emits the closing brace at column 0
            j = i + 1
            while j < len(lines) and lines[j].rstrip('\n') != '}':
                j += 1
            if j >= len(lines):
                raise AbiParseError(f"{name} in {path}: no closing brace found (unexpected emission shape)")
            body = [l.rstrip('\n') for l in lines[i + 1:j]]
            is_overlay = not name.startswith('gen_func_')
            ov_name = None
            if is_overlay:
                ov_name = name.split('_gen_')[0]
            return FoundFunction(
                name=name, addr=faddr, path=path,
                start_line=i + 1, end_line=j + 1,
                body=body, is_overlay=is_overlay, ov_name=ov_name,
            )
    raise AbiParseError(
        f"no gen body for address 0x{addr} found under {gen_dir} "
        f"(searched gen_func_{addr} and *_gen_{addr} across {len(candidates)} files)"
    )


# --------------------------------------------------------------------------------------------------
# Line-level idiom patterns (recompiler emission dialect)
# --------------------------------------------------------------------------------------------------

FRAME_OPEN_RE = re.compile(r'^\s*c->r\[29\] = c->r\[29\] \+ \(uint32_t\)-(\d+);\s*$')
FRAME_CLOSE_RE = re.compile(r'^\s*c->r\[29\] = c->r\[29\] \+ \(uint32_t\)(\d+); return;\s*$')
LABEL_RE = re.compile(r'^\s*(L_[0-9A-Fa-f]+):;\s*$')

# sp-relative store, any width. Group: width(8/16/32), offset (signed decimal), source expr.
SP_STORE_RE = re.compile(
    r'c->mem_w(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\), (.+?)\);'
)
# sp-relative load into a register (restore candidate). Group: dest reg, width, offset.
SP_LOAD_RE = re.compile(
    r'^\s*c->r\[(\d+)\] = \(uint32_t\)c->mem_r(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\)\);\s*$'
    r'|^\s*c->r\[(\d+)\] = c->mem_r(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\)\);\s*$'
)

# INDIRECT sp-relative access: the recompiler sometimes materializes an sp-relative address into a
# scratch register first (`c->r[2] = c->r[29] + (uint32_t)4;`) and stores/loads through THAT register
# on a later line (`c->mem_w32((c->r[2] + (uint32_t)0), ...);`). Seen e.g. in ov_a00_gen_8013FB88's
# per-branch inline scratch (the mode==1/mode==2 SZ-scratch idiom) — without tracking this, those
# stores are invisible to the sp-relative store scan even though they ARE guest-stack-frame content.
SP_ADDR_MATERIALIZE_RE = re.compile(r'^\s*c->r\[(\d+)\] = c->r\[29\] \+ \(uint32_t\)(-?\d+);\s*$')
INDIRECT_SP_STORE_RE = re.compile(
    r'c->mem_w(8|16|32)\(\(c->r\[(\d+)\] \+ \(uint32_t\)(-?\d+)\), (.+?)\);'
)
INDIRECT_SP_LOAD_RE = re.compile(
    r'^\s*c->r\[(\d+)\] = (?:\(uint32_t\))?c->mem_r(8|16|32)\(\(c->r\[(\d+)\] \+ \(uint32_t\)(-?\d+)\)\);\s*$'
)

# r31 (ra) set to a literal constant — always precedes a real call in this dialect.
RA_SET_RE = re.compile(r'c->r\[31\] = 0x([0-9A-Fa-f]+)u;')

# Direct intra-shard / intra-overlay calls: func_XXXXXXXX(c) or ov_<area>_func_XXXXXXXX(c)
CALL_RE = re.compile(r'\b((?:ov_[a-z0-9]+_func_|func_)([0-9A-Fa-f]{8}))\(c\);')
# Indirect dispatch: rec_dispatch(c, <expr>);
RECDISP_RE = re.compile(r'rec_dispatch\(c, ([^)]+)\);')

# Plain register-to-register assignment: c->r[N] = <expr>;  (used to track callee-saved liveness)
REG_ASSIGN_RE = re.compile(r'^\s*c->r\[(\d+)\] = (.+);\s*$')

CALLEE_SAVED = list(range(16, 24)) + [30]  # r16..r23, r30 (s8/fp) — MIPS o32 callee-saved (ra=r31 separate)


@dataclass
class StoreRecord:
    offset: int
    width: int
    source: str
    label: str            # label context ("entry" or the last-seen L_xxxx before this line)
    line_no: int           # index into body
    kind: str = "store"    # "prologue_spill" | "epilogue_restore" | "scratch"
    reg: Optional[int] = None   # if source/dest is a bare c->r[N], which N


@dataclass
class CallSite:
    line_no: int
    label: str
    target: str             # func name / rec_dispatch target expr
    kind: str                # "direct" | "rec_dispatch" | "switch_default_rec_dispatch"
    ra_const: Optional[str]  # hex string w/o 0x, or None if missing (BUG SMELL)
    live_regs: dict = field(default_factory=dict)  # {reg_num: last_write_expr}


@dataclass
class Contract:
    name: str
    addr: str
    path: str
    is_overlay: bool
    ov_name: Optional[str]
    frame_size: int                       # 0 if leafless (no sp descent)
    frame_close_sizes: list               # every observed ascent value (should all equal frame_size)
    prologue_spills: list                 # StoreRecord, program order
    epilogue_restores: list               # (reg, offset) pairs found in the tail restore block
    scratch_stores: list                  # StoreRecord, program order — the "everything else" bucket
    call_sites: list                      # CallSite, program order
    own_callee_saved_used: list           # which of CALLEE_SAVED + [31] this function itself spills/uses

    def to_json(self) -> dict:
        d = asdict(self)
        return d


def _decompound_line(raw: str) -> list:
    """The recompiler emits MIPS branch-delay-slot instructions inline in a compound statement:
        { int _t = (COND); DELAY_INSN; if (_t) goto LABEL; }
    DELAY_INSN always executes (branch delay slot) regardless of COND. Split such a line into
    the DELAY_INSN (as an unconditional statement) followed by a synthetic 'goto LABEL' guarded
    marker — for our purposes we only need to walk DELAY_INSN as an ordinary statement in program
    order and record the branch existed. Return a list of pseudo-statement strings, in the ORDER
    the guest actually executes them (delay insn first, branch decision second)."""
    m = re.match(r'^\s*\{ int _t = \(.+?\); (.+?); if \(_t\) goto (L_[0-9A-Fa-f]+); \}\s*$', raw)
    if m:
        delay_insn, target = m.group(1), m.group(2)
        return [delay_insn + ';', f'/*branch-> {target}*/']
    # Some compounds have no delay insn (pure conditional branch), e.g.:
    #   { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_..; }
    m2 = re.match(r'^\s*\{ int _t = \(.+?\);\s*if \(_t\) goto (L_[0-9A-Fa-f]+); \}\s*$', raw)
    if m2:
        return [f'/*branch-> {m2.group(1)}*/']
    return [raw]


def parse_contract(fn: FoundFunction) -> Contract:
    frame_size = 0
    frame_close_sizes = []
    prologue_spills = []
    epilogue_restores = []
    scratch_stores = []
    call_sites = []

    label = "entry"
    seen_first_label = False
    pending_ra = None          # (const_hex, line_no)
    last_write = {}            # reg -> (expr, line_no) for CALLEE_SAVED liveness tracking
    sp_addr_regs = {}          # reg -> sp offset, for the "materialize address, store through it" idiom
    saw_frame_open = False
    body_end_idx = len(fn.body)

    # Flatten delay-slot compounds into a pseudo-statement stream, remembering original line index.
    stream = []  # (orig_line_no, stmt_text)
    for i, raw in enumerate(fn.body):
        for stmt in _decompound_line(raw.strip()):
            stream.append((i, stmt))

    for line_no, stmt in stream:
        lm = LABEL_RE.match(stmt if stmt.endswith(':;') else '')
        if stmt.endswith(':;'):
            lm2 = re.match(r'^(L_[0-9A-Fa-f]+):;$', stmt)
            if lm2:
                label = lm2.group(1)
                seen_first_label = True
                continue

        if not saw_frame_open:
            fo = FRAME_OPEN_RE.match(stmt)
            if fo:
                frame_size = int(fo.group(1))
                saw_frame_open = True
                continue

        fc = FRAME_CLOSE_RE.match(stmt)
        if fc:
            frame_close_sizes.append(int(fc.group(1)))
            continue

        # address materialization: c->r[N] = c->r[29] + (uint32_t)K;  -- remember for indirect access,
        # then fall through to the normal REG_ASSIGN/CALLEE_SAVED liveness bookkeeping below (do NOT
        # 'continue' here — some functions do use a materialized address reg as a callee-saved value).
        am_sp = SP_ADDR_MATERIALIZE_RE.match(stmt)
        if am_sp:
            sp_addr_regs[int(am_sp.group(1))] = int(am_sp.group(2))

        # sp-relative store (direct)?
        sm = SP_STORE_RE.search(stmt)
        # sp-relative store (indirect through a materialized address register)?
        ism = None if sm else INDIRECT_SP_STORE_RE.search(stmt)
        if sm or (ism and int(ism.group(2)) in sp_addr_regs):
            if sm:
                width, off, src = int(sm.group(1)), int(sm.group(2)), sm.group(3)
            else:
                width = int(ism.group(1))
                off = sp_addr_regs[int(ism.group(2))] + int(ism.group(3))
                src = ism.group(4)
            reg_m = re.match(r'^c->r\[(\d+)\]$', src)
            rec = StoreRecord(offset=off, width=width, source=src, label=label,
                               line_no=line_no, reg=int(reg_m.group(1)) if reg_m else None)
            if not seen_first_label and reg_m is not None and sm is not None:
                rec.kind = "prologue_spill"
                prologue_spills.append(rec)
            else:
                rec.kind = "scratch"
                scratch_stores.append(rec)
            continue

        # sp-relative restore (register load, direct)?
        slm = SP_LOAD_RE.match(stmt)
        # sp-relative restore (indirect through a materialized address register)?
        islm = None if slm else INDIRECT_SP_LOAD_RE.match(stmt)
        if slm:
            g = slm.groups()
            if g[0] is not None:
                reg, width, off = int(g[0]), int(g[1]), int(g[2])
            else:
                reg, width, off = int(g[3]), int(g[4]), int(g[5])
            epilogue_restores.append((reg, off, width, line_no))
            last_write[reg] = (stmt, line_no)
            continue
        if islm and int(islm.group(3)) in sp_addr_regs:
            reg, width = int(islm.group(1)), int(islm.group(2))
            off = sp_addr_regs[int(islm.group(3))] + int(islm.group(4))
            epilogue_restores.append((reg, off, width, line_no))
            last_write[reg] = (stmt, line_no)
            continue

        # r31 (ra) constant set — remember for the next call site
        ram = RA_SET_RE.search(stmt)
        if ram:
            pending_ra = (ram.group(1), line_no)
            # r31 write itself also counts as a plain reg assign for liveness bookkeeping (rare to matter)

        # plain register assignment -> liveness tracking for callee-saved regs, and invalidate any
        # stale sp-address-materialization entry for a register that just got reassigned to something
        # else (unless this line WAS the materialization itself, handled above).
        for am in REG_ASSIGN_RE.finditer(stmt):
            reg = int(am.group(1))
            if reg in CALLEE_SAVED:
                last_write[reg] = (am.group(2), line_no)
            if reg in sp_addr_regs and not (am_sp and int(am_sp.group(1)) == reg):
                del sp_addr_regs[reg]

        # direct call?
        cm = CALL_RE.search(stmt)
        if cm:
            target = cm.group(1)
            live = {r: last_write[r][0] for r in CALLEE_SAVED if r in last_write}
            call_sites.append(CallSite(
                line_no=line_no, label=label, target=target, kind="direct",
                ra_const=pending_ra[0] if pending_ra else None, live_regs=live,
            ))
            pending_ra = None
            continue

        # rec_dispatch(c, expr) direct call in a statement
        rm = RECDISP_RE.search(stmt)
        if rm and 'default:' not in stmt:
            target = rm.group(1)
            live = {r: last_write[r][0] for r in CALLEE_SAVED if r in last_write}
            call_sites.append(CallSite(
                line_no=line_no, label=label, target=f"rec_dispatch({target})", kind="rec_dispatch",
                ra_const=pending_ra[0] if pending_ra else None, live_regs=live,
            ))
            pending_ra = None
            continue

        # switch(...) { case ...: goto ...; default: rec_dispatch(c, expr); return; } }
        if 'default: rec_dispatch(c,' in stmt:
            rm2 = re.search(r'default: rec_dispatch\(c, ([^)]+)\);', stmt)
            if rm2:
                live = {r: last_write[r][0] for r in CALLEE_SAVED if r in last_write}
                call_sites.append(CallSite(
                    line_no=line_no, label=label, target=f"rec_dispatch({rm2.group(1)})",
                    kind="switch_default_rec_dispatch",
                    ra_const=pending_ra[0] if pending_ra else None, live_regs=live,
                ))
                pending_ra = None

    if not saw_frame_open and not frame_close_sizes:
        # Leaf function with no sp frame at all — valid, not an error.
        frame_size = 0

    if frame_close_sizes and saw_frame_open:
        bad = [s for s in frame_close_sizes if s != frame_size]
        if bad:
            raise AbiParseError(
                f"{fn.name}: frame OPEN size {frame_size} does not match CLOSE size(s) {frame_close_sizes} "
                f"— unrecognized frame idiom, refusing to emit a possibly-wrong contract"
            )

    # cross-check: prologue spill offsets should be restored at matching offsets in epilogue_restores
    # (best-effort note only; not fatal — some functions spill more than they restore across branches)
    own_callee_saved_used = sorted({r.reg for r in prologue_spills if r.reg is not None})

    return Contract(
        name=fn.name, addr=fn.addr, path=fn.path, is_overlay=fn.is_overlay, ov_name=fn.ov_name,
        frame_size=frame_size, frame_close_sizes=frame_close_sizes,
        prologue_spills=prologue_spills, epilogue_restores=epilogue_restores,
        scratch_stores=scratch_stores, call_sites=call_sites,
        own_callee_saved_used=own_callee_saved_used,
    )


# --------------------------------------------------------------------------------------------------
# --contract rendering
# --------------------------------------------------------------------------------------------------

def render_contract_text(c: Contract) -> str:
    out = []
    out.append(f"# ABI contract: {c.name}  (0x{c.addr})")
    out.append(f"# source: {c.path}")
    if c.is_overlay:
        out.append(f"# overlay: {c.ov_name}")
    out.append("")
    out.append(f"frame_size = {c.frame_size}"
                + ("  (no sp descent — leaf/no-frame function)" if c.frame_size == 0 else ""))
    if c.frame_close_sizes:
        out.append(f"frame_close_sizes = {c.frame_close_sizes}  (exits, should all == frame_size)")
    out.append("")

    out.append(f"## prologue spills ({len(c.prologue_spills)}), sp-relative, program order")
    for s in c.prologue_spills:
        out.append(f"  sp+{s.offset:<4} <- r{s.reg:<2}  (w{s.width})")
    if not c.prologue_spills:
        out.append("  (none)")
    out.append("")

    out.append(f"## epilogue restores ({len(c.epilogue_restores)})")
    for reg, off, width, line_no in c.epilogue_restores:
        matched = any(s.reg == reg and s.offset == off for s in c.prologue_spills)
        tag = "" if matched else "  [!] no matching prologue spill at this offset"
        out.append(f"  r{reg:<2} <- sp+{off:<4}  (w{width}){tag}")
    if not c.epilogue_restores:
        out.append("  (none)")
    out.append("")

    out.append(f"## scratch / local sp-relative stores ({len(c.scratch_stores)}), program order, "
                f"with branch-label context")
    for s in c.scratch_stores:
        out.append(f"  [{s.label}] sp+{s.offset:<5} <- {s.source}   (w{s.width})")
    if not c.scratch_stores:
        out.append("  (none)")
    out.append("")

    out.append(f"## call sites ({len(c.call_sites)}), program order")
    for i, cs in enumerate(c.call_sites):
        ra = f"0x{cs.ra_const}u" if cs.ra_const else "MISSING <-- bug smell: no c->r[31] set before this call"
        out.append(f"  [{i}] label={cs.label} kind={cs.kind} target={cs.target}")
        out.append(f"       c->r[31] = {ra}")
        if cs.live_regs:
            live_str = ", ".join(f"r{r}={expr}" for r, expr in sorted(cs.live_regs.items()))
            out.append(f"       live callee-saved regs (heuristic, last-write-before-call): {live_str}")
        else:
            out.append(f"       live callee-saved regs: (none written this function before this call)")
    if not c.call_sites:
        out.append("  (none)")
    out.append("")

    out.append(f"## this function's own callee-saved footprint: r{c.own_callee_saved_used}")
    return "\n".join(out)


# --------------------------------------------------------------------------------------------------
# --scaffold rendering
# --------------------------------------------------------------------------------------------------

def _class_name_guess(c: Contract) -> str:
    return f"Frame_{c.addr}"


def render_scaffold(c: Contract) -> str:
    cname = _class_name_guess(c)
    out = []
    out.append(f"// Guest-stack frame RAII for {c.name} (0x{c.addr}), mirroring its real "
               f"`addiu sp,-{c.frame_size}` prologue.")
    out.append(f"// Generated by tools/abi_extract.py --scaffold 0x{c.addr} — VERIFY against "
               f"generated/... line-by-line before trusting (docs/fleet-workflow.md §9).")
    if c.frame_size == 0:
        out.append(f"// NOTE: no sp descent detected — this function has no guest-stack frame of its own.")
        out.append(f"// No Frame struct needed; only the call-site r31/liveness block below applies.")
    else:
        out.append(f"struct {cname} {{")
        out.append(f"  Core* c;")
        fields = []
        for s in c.prologue_spills:
            fields.append(f"r{s.reg}" if s.reg != 31 else "ra")
        decl = ", ".join(f"s{f}" if f != "ra" else "sra" for f in fields)
        out.append(f"  uint32_t {decl};" if decl else "  // (no register spills — sp-adjust only)")
        init = ", ".join(
            (f"s{f}(c_->r[{s.reg}])" if f != "ra" else f"sra(c_->r[31])")
            for f, s in zip(fields, c.prologue_spills)
        )
        out.append(f"  explicit {cname}(Core* c_) : c(c_){', ' + init if init else ''} {{")
        out.append(f"    c->r[29] -= {c.frame_size};")
        for f, s in zip(fields, c.prologue_spills):
            var = f"s{f}" if f != "ra" else "sra"
            out.append(f"    c->mem_w32(c->r[29] + {s.offset}, {var});")
        out.append(f"  }}")
        out.append(f"  ~{cname}() {{")
        for f, s in zip(fields, c.prologue_spills):
            dst = f"c->r[{s.reg}]"
            out.append(f"    {dst} = c->mem_r32(c->r[29] + {s.offset});")
        out.append(f"    c->r[29] += {c.frame_size};")
        out.append(f"  }}")
        out.append(f"}};")
    out.append("")
    out.append(f"// Call sites ({len(c.call_sites)}) — set c->r[31] to the EXACT RE'd constant before each,")
    out.append(f"// and mirror any callee-saved regs shown live (this function keeps them in the real")
    out.append(f"// register file across the call per MIPS calling convention; a callee that spills its")
    out.append(f"// caller's r16..r23/r30/ra at its own prologue needs the REAL value, not a stale one —")
    out.append(f"// see CLAUDE.md 'MIRROR THE GUEST STACK' + docs/findings/render.md's cmdListDispatch bug).")
    for i, cs in enumerate(c.call_sites):
        ra = f"0x{cs.ra_const}u" if cs.ra_const else "0x????????u /* MISSING in source — investigate */"
        out.append(f"// call site {i} [{cs.label}] -> {cs.target}")
        out.append(f"c->r[31] = {ra};")
        for r, expr in sorted(cs.live_regs.items()):
            out.append(f"c->r[{r}] = {expr};  // mirror gen's live value before this call")
        out.append(f"// {cs.target}(c);")
        out.append("")
    return "\n".join(out)


# --------------------------------------------------------------------------------------------------
# --audit (heuristic, best-effort)
# --------------------------------------------------------------------------------------------------

def render_audit(c: Contract, native_path: str) -> str:
    if not os.path.isfile(native_path):
        raise AbiParseError(f"--audit target not found: {native_path}")
    with open(native_path, 'r', errors='replace') as f:
        text = f.read()

    out = []
    out.append(f"# HEURISTIC audit of {native_path} against {c.name} (0x{c.addr})")
    out.append("# This is a grep-level best-effort check, NOT a substitute for the mandatory")
    out.append("# line-by-line RE verify pass (docs/fleet-workflow.md §9). False negatives/positives")
    out.append("# are expected — e.g. constants may appear via a named constexpr instead of the literal.")
    out.append("")

    warnings = []
    oks = []

    if c.frame_size:
        # look for the frame size appearing near r[29] adjustments
        if re.search(rf'\br\[29\]\s*[-+]=\s*{c.frame_size}\b', text) or \
           re.search(rf'-\s*{c.frame_size}\b', text):
            oks.append(f"frame size {c.frame_size} literal found somewhere in file")
        else:
            warnings.append(f"frame size {c.frame_size} NOT found as a literal anywhere — "
                             f"is the sp descent/ascent mirrored?")
    else:
        oks.append("no frame required (leaf) — nothing to check for sp descent")

    for s in c.prologue_spills:
        if re.search(rf'\b{s.offset}\b', text):
            oks.append(f"spill offset {s.offset} (r{s.reg}) literal present")
        else:
            warnings.append(f"spill offset {s.offset} (r{s.reg}) NOT found as a literal — "
                             f"is this callee-save spilled?")

    for i, cs in enumerate(c.call_sites):
        if cs.ra_const is None:
            warnings.append(f"call site {i} ({cs.target}): source has NO r31 constant either "
                             f"(nothing to check)")
            continue
        needle = f"0x{cs.ra_const}"
        if re.search(re.escape(needle), text, re.IGNORECASE):
            oks.append(f"call site {i}: ra constant 0x{cs.ra_const} literal present")
        else:
            warnings.append(f"call site {i} ({cs.target}): ra constant 0x{cs.ra_const} "
                             f"NOT found literally — is c->r[31] set before this call?")
        for r in cs.live_regs:
            if re.search(rf'r\[{r}\]', text):
                oks.append(f"call site {i}: r{r} liveness — c->r[{r}] appears SOMEWHERE in file "
                            f"(not call-site-scoped; verify manually)")
            else:
                warnings.append(f"call site {i} ({cs.target}): r{r} is live in source but "
                                 f"c->r[{r}] never appears in native file — likely missing "
                                 f"register-faithfulness mirror (the cmdListDispatch bug class)")

    out.append(f"OK ({len(oks)}):")
    for o in oks:
        out.append(f"  [ok] {o}")
    out.append("")
    out.append(f"WARNINGS ({len(warnings)}) — heuristic, verify by hand:")
    for w in warnings:
        out.append(f"  [!!] {w}")
    if not warnings:
        out.append("  (none — but this heuristic can still miss real bugs; do the RE verify pass anyway)")
    return "\n".join(out)


# --------------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('addr', help='guest address, hex, e.g. 8003CDD8 or 0x8003CDD8')
    ap.add_argument('--contract', action='store_true', help='emit the ABI contract (default action)')
    ap.add_argument('--scaffold', action='store_true', help='emit a C++ Frame RAII + call-site scaffold')
    ap.add_argument('--audit', metavar='NATIVE_CPP', help='heuristic audit of an existing native port')
    ap.add_argument('--json', action='store_true', help='emit --contract as JSON instead of text')
    ap.add_argument('--repo', default=None, help='repo root (default: dir containing this script\'s parent)')
    args = ap.parse_args(argv)

    repo = args.repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    try:
        fn = locate_function(repo, args.addr)
        contract = parse_contract(fn)

        did_something = False
        if args.scaffold:
            print(render_scaffold(contract))
            did_something = True
        if args.audit:
            print(render_audit(contract, args.audit))
            did_something = True
        if args.contract or not did_something:
            if args.json:
                print(json.dumps(contract.to_json(), indent=2))
            else:
                print(render_contract_text(contract))
    except AbiParseError as e:
        print(f"abi_extract: FAILED — {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
