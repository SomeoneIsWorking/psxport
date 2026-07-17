#!/usr/bin/env python3
"""abi_extract.py — static ABI/stack-contract extractor for recompiled gen function bodies.

Purpose (see CLAUDE.md "MIRROR THE GUEST STACK" + docs/faithful-execution.md): the #1 recurring bug
class when hand-porting a `gen_func_<addr>` / `ov_<area>_gen_<addr>` body to a native C++ class method
is getting the guest-stack frame / callee-save spill / r31-before-call / callee-saved-register-liveness
boilerplate wrong. This tool parses the RECOMPILED C TEXT (ground truth — never Ghidra's pseudo-C,
which garbles COP2/delay slots) and emits:

  1. --contract (default): a human-readable + JSON-able description of the function's guest-ABI
     contract: frame size, ordered sp-relative stores (prologue spills / scratch stores) with their
     CFG branch context, and every call site with its target, its r31 (return-address) constant, and
     which callee-saved registers (r16..r23, r30) are live going into it — computed over the
     function's real control-flow graph, not file order.
  2. --scaffold: a ready-to-paste C++ Frame RAII struct + call-site comment block in the house style
     (game/render/perobj_dispatch.cpp's CmdListFrame / game/world/object_table.cpp's frame idiom).
  3. --audit <native.cpp>: best-effort heuristic grep-level check of an EXISTING native port against
     the contract (frame delta present? ra constants present? spill offsets present?). This is
     intentionally noisy/heuristic — labelled as such in the output; it is not a substitute for the
     line-by-line RE verify pass docs/fleet-workflow.md §9 requires.

This is a STATIC TEXT parser over the recompiler's fixed emission idioms, plus a real basic-block CFG
built from those idioms (labels, conditional/unconditional gotos, the switch-dispatch jump-table shape,
and function returns — see docs/abi-extract.md for the enumerated corpus shapes). It recognizes the
emission patterns actually seen in generated/*.c; if it hits a function body whose shape doesn't match
those idioms, it FAILS LOUDLY (raises AbiParseError) rather than emit a guessed/wrong contract — a wrong
contract is worse than none (see CLAUDE.md "No bandaids").

Usage:
    python3 tools/abi_extract.py <hexaddr> [--contract|--scaffold|--audit] [--json] [--repo <path>]

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

FRAME_OPEN_RE = re.compile(r'^c->r\[29\] = c->r\[29\] \+ \(uint32_t\)-(\d+);$')
FRAME_CLOSE_RE = re.compile(r'^c->r\[29\] = c->r\[29\] \+ \(uint32_t\)(\d+); return;$')
LABEL_FULL_RE = re.compile(r'^(L_[0-9A-Fa-f]+):;$')
# A label immediately followed by a statement on the SAME physical line (the recompiler does this
# when a jump target coincides with the first instruction of what would otherwise be a plain
# statement line — verified 4030 corpus instances, always exactly one label + one trailing statement,
# never chained/multi-label). e.g. `L_8013B07C:; c->r[2] = c->r[2] << 16;`
LABEL_PREFIX_RE = re.compile(r'^(L_[0-9A-Fa-f]+):;\s*(\S.*)$')

# MIPS branch-delay-slot compound: { int _t = (COND); DELAY_INSN; if (_t) goto LABEL; }
# DELAY_INSN always executes (branch delay slot) regardless of COND; verified over the whole
# generated/ corpus (47524 instances) that the delay part is always a single statement (no embedded
# ';') — see docs/abi-extract.md "corpus shapes".
COMPOUND_DELAY_RE = re.compile(r'^\{ int _t = \((.+?)\); (.+?); if \(_t\) goto (L_[0-9A-Fa-f]+); \}$')
# Compound with no delay insn (pure conditional branch).
COMPOUND_NODELAY_RE = re.compile(r'^\{ int _t = \((.+?)\); if \(_t\) goto (L_[0-9A-Fa-f]+); \}$')

# Unconditional goto, optionally preceded by exactly one plain statement on the same line
# ("STMT; goto L;"); verified over the corpus that this shape never carries more than one leading
# statement. Order matters: try the two-part shape first, then the bare shape.
GOTO_SUFFIX_RE = re.compile(r'^(.+); goto (L_[0-9A-Fa-f]+);$')
BARE_GOTO_RE = re.compile(r'^goto (L_[0-9A-Fa-f]+);$')

# Bare return, or "STMT; return;" (a tail call/dispatch immediately followed by return — very common:
# ~17k instances in the corpus). FRAME_CLOSE_RE (sp ascent + return) is checked first since it's a
# more specific variant of this same shape.
BARE_RETURN_RE = re.compile(r'^return;$')
RETURN_SUFFIX_RE = re.compile(r'^(.+); return;$')

# Switch-dispatch jump table: {  switch (EXPR) { case C: goto L; ... default: DEFAULT; return; } }
# This is the recompiler's coroutine/state-resume dispatch idiom (verified uniform shape across all
# 1041 corpus instances: N "case CONST: goto LABEL;" clauses then one "default: STMT; return;").
SWITCH_RE = re.compile(r'^\{ {1,2}switch \((.+?)\) \{ (.*) default: (.+?); return; \} \}$')
SWITCH_CASE_RE = re.compile(r'case ([^:]+): goto (L_[0-9A-Fa-f]+);')

# sp-relative store, any width. Group: width(8/16/32), offset (signed decimal), source expr.
SP_STORE_RE = re.compile(
    r'c->mem_w(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\), (.+?)\);'
)
# sp-relative load into a register (restore candidate). Group: dest reg, width, offset.
SP_LOAD_RE = re.compile(
    r'^c->r\[(\d+)\] = \(uint32_t\)c->mem_r(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\)\);$'
    r'|^c->r\[(\d+)\] = c->mem_r(8|16|32)\(\(c->r\[29\] \+ \(uint32_t\)(-?\d+)\)\);$'
)

# INDIRECT sp-relative access: the recompiler sometimes materializes an sp-relative address into a
# scratch register first (`c->r[2] = c->r[29] + (uint32_t)4;`) and stores/loads through THAT register
# on a later line (`c->mem_w32((c->r[2] + (uint32_t)0), ...);`). Seen e.g. in ov_a00_gen_8013FB88's
# per-branch inline scratch (the mode==1/mode==2 SZ-scratch idiom). Tracking this idiom PER CFG BLOCK
# (via the reaching-definitions dataflow below) — not globally in file order — is what fixes the
# previously-missed sp+12 slot: r2's materialized value at block entry now comes from the actual CFG
# predecessor edge(s) reaching that block, not from whatever a differently-ordered-in-the-FILE sibling
# block happened to leave behind.
SP_ADDR_MATERIALIZE_RE = re.compile(r'^c->r\[(\d+)\] = c->r\[29\] \+ \(uint32_t\)(-?\d+);$')
INDIRECT_SP_STORE_RE = re.compile(
    r'c->mem_w(8|16|32)\(\(c->r\[(\d+)\] \+ \(uint32_t\)(-?\d+)\), (.+?)\);'
)
INDIRECT_SP_LOAD_RE = re.compile(
    r'^c->r\[(\d+)\] = (?:\(uint32_t\))?c->mem_r(8|16|32)\(\(c->r\[(\d+)\] \+ \(uint32_t\)(-?\d+)\)\);$'
)

# r31 (ra) set to a literal constant — always precedes a real call in this dialect.
RA_SET_RE = re.compile(r'c->r\[31\] = 0x([0-9A-Fa-f]+)u;')

# Direct intra-shard / intra-overlay calls: func_XXXXXXXX(c) or ov_<area>_func_XXXXXXXX(c)
CALL_RE = re.compile(r'\b((?:ov_[a-z0-9]+_func_|func_)([0-9A-Fa-f]{8}))\(c\);')
# Indirect dispatch: rec_dispatch(c, <expr>);
RECDISP_RE = re.compile(r'rec_dispatch\(c, ([^)]+)\);')

# Plain register-to-register assignment: c->r[N] = <expr>;  (used to track callee-saved liveness)
REG_ASSIGN_RE = re.compile(r'^c->r\[(\d+)\] = (.+);$')

CALLEE_SAVED = list(range(16, 24)) + [30]  # r16..r23, r30 (s8/fp) — MIPS o32 callee-saved (ra=r31 separate)

# Address-agnostic memory-write matcher (ANY c->mem_wNN(...) call, sp-relative or not) — used by
# tools/port_check.py's op-sequence extraction. Deliberately just the width, not the address: address
# expressions can be arbitrarily reformatted by a faithful rename without changing behavior, but a
# missing/extra/reordered/width-changed store is a real semantic difference.
MEM_W_ANY_RE = re.compile(r'c->mem_w(8|16|32)\(')


# --------------------------------------------------------------------------------------------------
# Tokenizer: raw body lines -> a flat instruction-level token stream (branch-delay compounds
# decomposed into their real execution order, terminator statements split from any leading stmt).
# --------------------------------------------------------------------------------------------------

@dataclass
class Tok:
    kind: str        # 'label' | 'stmt' | 'condgoto' | 'goto' | 'return' | 'switch'
    line_no: int      # index into fn.body
    text: str = ""            # for 'stmt'
    target: str = ""          # for 'condgoto' / 'goto'
    cond_text: str = ""       # for 'condgoto' (source condition expression, for human-readable context)
    frame_close: Optional[int] = None   # for 'return', if this was the frame-ascent+return combo
    cases: list = field(default_factory=list)   # for 'switch': [(case_val, target_label), ...]
    default_text: str = ""    # for 'switch'


def tokenize(body: list) -> list:
    toks = []
    for i, raw in enumerate(body):
        stmt = raw.strip()
        if not stmt:
            continue

        lm = LABEL_FULL_RE.match(stmt)
        if lm:
            toks.append(Tok(kind='label', line_no=i, target=lm.group(1)))
            continue

        lp = LABEL_PREFIX_RE.match(stmt)
        if lp:
            toks.append(Tok(kind='label', line_no=i, target=lp.group(1)))
            toks.extend(_classify_stmt(lp.group(2), i))
            continue

        toks.extend(_classify_stmt(stmt, i))
    return toks


def _classify_stmt(stmt: str, i: int) -> list:
    """Classify one (label-stripped) physical-line statement into its token(s)."""
    fc = FRAME_CLOSE_RE.match(stmt)
    if fc:
        return [Tok(kind='return', line_no=i, frame_close=int(fc.group(1)))]

    if BARE_RETURN_RE.match(stmt):
        return [Tok(kind='return', line_no=i)]

    cd = COMPOUND_DELAY_RE.match(stmt)
    if cd:
        cond_text, delay_insn, target = cd.group(1), cd.group(2), cd.group(3)
        return [Tok(kind='stmt', line_no=i, text=delay_insn + ';'),
                Tok(kind='condgoto', line_no=i, target=target, cond_text=cond_text)]

    cn = COMPOUND_NODELAY_RE.match(stmt)
    if cn:
        cond_text, target = cn.group(1), cn.group(2)
        return [Tok(kind='condgoto', line_no=i, target=target, cond_text=cond_text)]

    sw = SWITCH_RE.match(stmt)
    if sw:
        expr, cases_part, default_text = sw.group(1), sw.group(2), sw.group(3)
        cases = SWITCH_CASE_RE.findall(cases_part)
        # sanity: the case list, re-joined, must reconstruct the source exactly (else this is an
        # emission shape we haven't seen — fail loudly rather than silently drop a case edge).
        rejoined = ''.join(f'case {c}: goto {t}; ' for c, t in cases)
        if rejoined.strip() != cases_part.strip():
            raise AbiParseError(
                f"line {i}: switch-dispatch case list doesn't round-trip cleanly "
                f"(unrecognized case shape) — refusing to emit a possibly-wrong contract: {stmt!r}"
            )
        return [Tok(kind='switch', line_no=i, cases=cases, default_text=default_text,
                     text=f"switch({expr})")]

    rs = RETURN_SUFFIX_RE.match(stmt)
    if rs:
        return [Tok(kind='stmt', line_no=i, text=rs.group(1) + ';'),
                Tok(kind='return', line_no=i)]

    gs = GOTO_SUFFIX_RE.match(stmt)
    if gs:
        return [Tok(kind='stmt', line_no=i, text=gs.group(1) + ';'),
                Tok(kind='goto', line_no=i, target=gs.group(2))]

    bg = BARE_GOTO_RE.match(stmt)
    if bg:
        return [Tok(kind='goto', line_no=i, target=bg.group(1))]

    # plain statement, falls through to the next physical line
    return [Tok(kind='stmt', line_no=i, text=stmt)]


# --------------------------------------------------------------------------------------------------
# CFG construction
# --------------------------------------------------------------------------------------------------

@dataclass
class Edge:
    target: str
    kind: str                    # 'fallthrough' | 'cond_taken' | 'cond_fallthrough' | 'uncond' |
                                  # 'switch_case'
    cond_text: str = ""
    case_val: str = ""


@dataclass
class Block:
    name: str
    stmts: list = field(default_factory=list)     # list of Tok(kind='stmt')
    out_edges: list = field(default_factory=list)  # list of Edge
    in_edges: list = field(default_factory=list)   # list of (pred_name, Edge)
    is_exit: bool = False        # ends in return (no successors)
    exit_frame_close: Optional[int] = None   # frame-ascent size fused into this block's return, if any
    orphan: bool = False         # created right after an uncond goto/return/switch: NOT entered by
                                  # fallthrough (real MIPS semantics — code after an unconditional
                                  # control transfer, before the next branch target, never executes
                                  # unless something else jumps to a label reusing this spot; see
                                  # docs/abi-extract.md). Must NOT get an implicit fallthrough edge
                                  # from a following label.
    switch_default_text: Optional[str] = None
    switch_default_line: Optional[int] = None


def build_cfg(toks: list) -> tuple:
    """Split the token stream into basic blocks + edges. Returns (blocks dict, order list, entry_name)."""
    blocks = {}
    order = []  # block names in the order first created, for stable iteration

    def new_block(name=None, _ctr=[0]):
        if name is None:
            _ctr[0] += 1
            name = f"__anon_{_ctr[0]}"
        if name not in blocks:
            blocks[name] = Block(name=name)
            order.append(name)
        return blocks[name]

    entry = new_block("entry")
    cur = entry

    def link(pred_name, edge: Edge):
        blocks[pred_name].out_edges.append(edge)

    for t in toks:
        if t.kind == 'label':
            target_block = new_block(t.target)
            if not cur.is_exit and not cur.orphan and cur.name != t.target:
                link(cur.name, Edge(target=t.target, kind='fallthrough'))
            cur = target_block
            continue

        if t.kind == 'stmt':
            cur.stmts.append(t)
            continue

        if t.kind == 'condgoto':
            link(cur.name, Edge(target=t.target, kind='cond_taken', cond_text=t.cond_text))
            nxt = new_block()
            link(cur.name, Edge(target=nxt.name, kind='cond_fallthrough', cond_text=t.cond_text))
            cur = nxt
            continue

        if t.kind == 'goto':
            link(cur.name, Edge(target=t.target, kind='uncond'))
            cur = new_block()
            cur.orphan = True  # dead placeholder unless a later label happens to be jumped to
            continue

        if t.kind == 'return':
            cur.is_exit = True
            cur.exit_frame_close = t.frame_close
            cur = new_block()
            cur.orphan = True  # dead placeholder — see above
            continue

        if t.kind == 'switch':
            for case_val, case_target in t.cases:
                link(cur.name, Edge(target=case_target, kind='switch_case', case_val=case_val))
            cur.switch_default_text = t.default_text
            cur.switch_default_line = t.line_no
            cur.is_exit = True  # default: STMT; return; — no fallthrough out of the switch
            cur = new_block()
            cur.orphan = True  # dead placeholder — see above
            continue

        raise AbiParseError(f"unrecognized token kind {t.kind!r} — internal tokenizer bug")

    for name, blk in blocks.items():
        for e in blk.out_edges:
            if e.target not in blocks:
                raise AbiParseError(
                    f"branch/goto target {e.target!r} from block {name!r} has no matching label in "
                    f"this function body — unrecognized emission shape, refusing to guess"
                )
            blocks[e.target].in_edges.append((name, e))

    return blocks, order, "entry"


# --------------------------------------------------------------------------------------------------
# CFG dataflow: reaching sp-address-materializations + reaching callee-saved register values
# --------------------------------------------------------------------------------------------------

def _reachable_blocks(blocks: dict, order: list, entry: str) -> list:
    """BFS from entry; blocks unreachable from entry (dead code after an unconditional
    goto/return with no label picking it back up) are dropped from analysis — they are, by
    construction, never executed."""
    seen_set = {entry}
    stack = [entry]
    while stack:
        n = stack.pop()
        for e in blocks[n].out_edges:
            if e.target not in seen_set:
                seen_set.add(e.target)
                stack.append(e.target)
    return [n for n in order if n in seen_set]


def _within_block_sp_addr_effects(blk: Block) -> dict:
    """Net effect of this block's own statements (in program order) on the sp-address-
    materialization map: reg -> sp-offset if this block (re)materializes it and it survives to the
    block's end, reg absent if the block never touches it, reg -> None if the block kills it
    (reassigns to something that isn't a materialization)."""
    effects = {}
    for t in blk.stmts:
        am = SP_ADDR_MATERIALIZE_RE.match(t.text)
        if am:
            effects[int(am.group(1))] = int(am.group(2))
            continue
        rm = REG_ASSIGN_RE.match(t.text)
        if rm:
            effects[int(rm.group(1))] = None
    return effects


def compute_reaching_sp_addr(blocks: dict, order: list, entry: str) -> dict:
    """Per-block ENTRY reaching-value map: reg -> sp-offset (only when EVERY predecessor path agrees;
    otherwise the reg is simply not known at block entry, matching how a real CFG dataflow "meet"
    (intersection) operates). Standard iterative fixpoint over the CFG (handles gt3's loop)."""
    reachable = _reachable_blocks(blocks, order, entry)
    in_state = {n: {} for n in reachable}   # entry-state per block
    changed = True
    iterations = 0
    while changed:
        changed = False
        iterations += 1
        if iterations > 10000:
            raise AbiParseError("sp-address dataflow did not converge — unexpected CFG shape")
        for n in reachable:
            blk = blocks[n]
            preds = [(p, e) for (p, e) in blk.in_edges if p in in_state]
            if not preds:
                merged = {} if n == entry else in_state[n]
            else:
                merged = None
                for p, _e in preds:
                    p_out = dict(in_state[p])
                    p_out.update(_within_block_sp_addr_effects(blocks[p]))
                    p_out = {r: v for r, v in p_out.items() if v is not None}
                    if merged is None:
                        merged = dict(p_out)
                    else:
                        merged = {r: v for r, v in merged.items() if p_out.get(r) == v}
            if merged != in_state[n]:
                in_state[n] = merged
                changed = True
    return in_state


def compute_reaching_callee_saved(blocks: dict, order: list, entry: str) -> dict:
    """Per-block ENTRY reaching-value map for CALLEE_SAVED regs: reg -> frozenset of distinct
    (expr) strings reaching this block along different CFG paths. size==1 => definite single value;
    size>1 => genuinely path-conditional (report per-path, don't collapse to one answer)."""
    reachable = _reachable_blocks(blocks, order, entry)
    in_state = {n: {} for n in reachable}   # reg -> frozenset[str]

    def block_effect(blk: Block) -> dict:
        eff = {}
        for t in blk.stmts:
            rm = REG_ASSIGN_RE.match(t.text)
            if rm:
                reg = int(rm.group(1))
                if reg in CALLEE_SAVED:
                    eff[reg] = frozenset([rm.group(2)])
        return eff

    block_defs = {n: block_effect(blocks[n]) for n in reachable}

    changed = True
    iterations = 0
    while changed:
        changed = False
        iterations += 1
        if iterations > 10000:
            raise AbiParseError("callee-saved dataflow did not converge — unexpected CFG shape")
        for n in reachable:
            blk = blocks[n]
            preds = [(p, e) for (p, e) in blk.in_edges if p in in_state]
            if not preds:
                merged = {} if n == entry else in_state[n]
            else:
                merged = {}
                all_regs = set()
                out_states = []
                for p, _e in preds:
                    p_out = dict(in_state[p])
                    p_out.update(block_defs[p])  # block's own defs override inherited values
                    out_states.append(p_out)
                    all_regs |= set(p_out.keys())
                for r in all_regs:
                    vals = set()
                    for st in out_states:
                        if r in st:
                            vals |= st[r]
                    defined_on_all = all(r in st for st in out_states)
                    if not defined_on_all:
                        # a reg not defined on every incoming path is, itself, a form of
                        # path-conditionality (whatever value it has depends on which path was
                        # taken) — surface it rather than silently reporting only the defined subset.
                        vals = vals | {"<undefined on some incoming path>"}
                    merged[r] = frozenset(vals)
            if merged != in_state[n]:
                in_state[n] = merged
                changed = True
    return in_state


# --------------------------------------------------------------------------------------------------
# Contract data model
# --------------------------------------------------------------------------------------------------

@dataclass
class StoreRecord:
    offset: int
    width: int
    source: str
    label: str             # enclosing block name
    line_no: int            # index into body
    kind: str = "store"     # "prologue_spill" | "scratch"
    reg: Optional[int] = None    # if source/dest is a bare c->r[N], which N
    guard_chain: list = field(default_factory=list)   # human-readable branch-context, entry -> this block


@dataclass
class CallSite:
    line_no: int
    label: str
    target: str              # func name / rec_dispatch target expr
    kind: str                 # "direct" | "rec_dispatch" | "switch_default_rec_dispatch"
    ra_const: Optional[str]   # hex string w/o 0x, or None if missing (BUG SMELL)
    live_regs: dict = field(default_factory=dict)     # {reg_num: single expr}      (definite)
    conditional_live_regs: dict = field(default_factory=dict)  # {reg_num: [expr, ...]} (path-dependent)


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
    epilogue_restores: list               # (reg, offset, width, line_no, block_name)
    scratch_stores: list                  # StoreRecord, CFG-block order — the "everything else" bucket
    call_sites: list                      # CallSite, program order
    own_callee_saved_used: list           # which of CALLEE_SAVED + [31] this function itself spills/uses
    unreachable_block_count: int = 0      # dead-code blocks dropped from analysis (informational)

    def to_json(self) -> dict:
        return asdict(self)


def _guard_chain_for(name: str, blocks: dict, max_hops: int = 8) -> list:
    """Walk backward via the unique-predecessor chain to build a short, readable description of
    which branch(es) guard reachability of this block. Stops at entry, at a merge point (>1
    predecessor — reported as such rather than picking one arbitrarily), or after max_hops."""
    chain = []
    cur = name
    hops = 0
    while hops < max_hops:
        blk = blocks[cur]
        preds = blk.in_edges
        if not preds:
            break  # entry, or an otherwise-unreached block
        if len(preds) > 1:
            chain.append(f"<= merge of {len(preds)} paths: " + ", ".join(p for p, _e in preds))
            break
        p, e = preds[0]
        if e.kind in ('cond_taken', 'cond_fallthrough'):
            taken = "true" if e.kind == 'cond_taken' else "false"
            chain.append(f"{p}: ({e.cond_text}) == {taken}")
        elif e.kind == 'switch_case':
            chain.append(f"{p}: switch case {e.case_val}")
        elif e.kind == 'uncond':
            chain.append(f"{p}: (unconditional goto)")
        else:
            chain.append(f"{p}: (fallthrough)")
        cur = p
        hops += 1
        if cur == 'entry':
            break
    chain.reverse()
    return chain


def parse_contract(fn: FoundFunction) -> Contract:
    toks = tokenize(fn.body)
    blocks, order, entry_name = build_cfg(toks)
    reachable = _reachable_blocks(blocks, order, entry_name)
    unreachable_count = len(order) - len(reachable)

    # ---- frame open/close ----
    # The `addiu sp,-N` prologue is not always the very first statement (the recompiler sometimes
    # emits a couple of register loads ahead of it) — scan the whole entry block, not just stmts[0],
    # matching the original tool's whole-body scan-until-found behavior.
    # Prologue block: normally `entry`, but some bodies open with a label on their very first
    # instruction (e.g. ov_a03_gen_8010F1A0's `L_8010F1A0:;` before the sp descent) which makes the
    # literal entry block EMPTY — walk forward through empty single-fallthrough blocks to the first
    # block that has statements; that block's code is still executed unconditionally at function
    # entry, so it is the prologue for spill-classification purposes.
    entry_blk = blocks[entry_name]
    while not entry_blk.stmts and len(entry_blk.out_edges) == 1 \
            and entry_blk.out_edges[0].kind == 'fallthrough':
        entry_blk = blocks[entry_blk.out_edges[0].target]
    frame_size = 0
    saw_frame_open = False
    frame_open_idx = None
    for idx, t in enumerate(entry_blk.stmts):
        fo = FRAME_OPEN_RE.match(t.text)
        if fo:
            frame_size = int(fo.group(1))
            saw_frame_open = True
            frame_open_idx = idx
            break

    # Frame closes come in two emitted spellings: the fused `sp-ascent; return;` single line
    # (captured on the exit block's return token) and the two-line form (a plain sp-ascent statement
    # as the LAST statement of an exit block — e.g. gen_func_80087A60). Only REACHABLE exit blocks
    # count: several gen bodies carry a dead, never-entered sibling function's code after their real
    # return (e.g. gen_func_800232F4/80079528), whose frame ops must not pollute — or false-fail —
    # this function's open/close consistency check.
    frame_close_sizes = []
    ascend_re = re.compile(r'^c->r\[29\] = c->r\[29\] \+ \(uint32_t\)(\d+);$')
    for n in reachable:
        blk = blocks[n]
        if not blk.is_exit:
            continue
        if blk.exit_frame_close is not None:
            frame_close_sizes.append(blk.exit_frame_close)
        elif blk.stmts:
            am = ascend_re.match(blk.stmts[-1].text)
            if am:
                frame_close_sizes.append(int(am.group(1)))

    if frame_close_sizes and saw_frame_open:
        bad = [s for s in frame_close_sizes if s != frame_size]
        if bad:
            raise AbiParseError(
                f"{fn.name}: frame OPEN size {frame_size} does not match CLOSE size(s) {frame_close_sizes} "
                f"— unrecognized frame idiom, refusing to emit a possibly-wrong contract"
            )

    # ---- reaching-value dataflow over the real CFG ----
    sp_addr_in = compute_reaching_sp_addr(blocks, order, entry_name)
    callee_in = compute_reaching_callee_saved(blocks, order, entry_name)

    # ---- prologue spills: unconditionally-executed stores in the entry block only, register-sourced,
    # after the frame-open statement (wherever it falls in the entry block) ----
    prologue_spills = []
    for t in (entry_blk.stmts[frame_open_idx + 1:] if saw_frame_open else []):
        sm = SP_STORE_RE.search(t.text)
        if sm:
            width, off, src = int(sm.group(1)), int(sm.group(2)), sm.group(3)
            reg_m = re.match(r'^c->r\[(\d+)\]$', src)
            if reg_m:
                prologue_spills.append(StoreRecord(
                    offset=off, width=width, source=src, label='entry', line_no=t.line_no,
                    kind='prologue_spill', reg=int(reg_m.group(1)), guard_chain=[],
                ))

    prologue_line_nos = {s.line_no for s in prologue_spills}

    # ---- per-block scan: scratch stores, epilogue restores, call sites ----
    scratch_stores = []
    epilogue_restores = []
    call_sites = []

    for n in reachable:
        blk = blocks[n]
        sp_addr_regs = dict(sp_addr_in.get(n, {}))
        callee_live = {r: set(vs) for r, vs in callee_in.get(n, {}).items()}
        guard_chain_cache = {}

        def get_guard_chain():
            if n not in guard_chain_cache:
                guard_chain_cache[n] = _guard_chain_for(n, blocks)
            return guard_chain_cache[n]

        pending_ra = None

        for t in blk.stmts:
            stmt = t.text

            am = SP_ADDR_MATERIALIZE_RE.match(stmt)
            if am:
                sp_addr_regs[int(am.group(1))] = int(am.group(2))

            sm = SP_STORE_RE.search(stmt)
            ism = None if sm else INDIRECT_SP_STORE_RE.search(stmt)
            if sm or (ism and int(ism.group(2)) in sp_addr_regs):
                if sm:
                    width, off, src = int(sm.group(1)), int(sm.group(2)), sm.group(3)
                else:
                    width = int(ism.group(1))
                    off = sp_addr_regs[int(ism.group(2))] + int(ism.group(3))
                    src = ism.group(4)
                if t.line_no not in prologue_line_nos:
                    reg_m = re.match(r'^c->r\[(\d+)\]$', src)
                    scratch_stores.append(StoreRecord(
                        offset=off, width=width, source=src, label=n, line_no=t.line_no,
                        kind='scratch', reg=int(reg_m.group(1)) if reg_m else None,
                        guard_chain=get_guard_chain(),
                    ))

            slm = SP_LOAD_RE.match(stmt)
            islm = None if slm else INDIRECT_SP_LOAD_RE.match(stmt)
            if slm:
                g = slm.groups()
                if g[0] is not None:
                    reg, width, off = int(g[0]), int(g[1]), int(g[2])
                else:
                    reg, width, off = int(g[3]), int(g[4]), int(g[5])
                epilogue_restores.append((reg, off, width, t.line_no, n))
            elif islm and int(islm.group(3)) in sp_addr_regs:
                reg, width = int(islm.group(1)), int(islm.group(2))
                off = sp_addr_regs[int(islm.group(3))] + int(islm.group(4))
                epilogue_restores.append((reg, off, width, t.line_no, n))

            ram = RA_SET_RE.search(stmt)
            if ram:
                pending_ra = (ram.group(1), t.line_no)

            for rm in REG_ASSIGN_RE.finditer(stmt):
                reg = int(rm.group(1))
                if reg in CALLEE_SAVED:
                    callee_live[reg] = {rm.group(2)}
                if reg in sp_addr_regs and not (am and int(am.group(1)) == reg):
                    del sp_addr_regs[reg]

            cm = CALL_RE.search(stmt)
            rm2 = None if cm else RECDISP_RE.search(stmt)
            if cm or (rm2 and 'default:' not in stmt):
                if cm:
                    target, kind = cm.group(1), "direct"
                else:
                    target, kind = f"rec_dispatch({rm2.group(1)})", "rec_dispatch"
                live, cond_live = _split_definite_conditional(callee_live)
                call_sites.append(CallSite(
                    line_no=t.line_no, label=n, target=target, kind=kind,
                    ra_const=pending_ra[0] if pending_ra else None,
                    live_regs=live, conditional_live_regs=cond_live,
                ))
                pending_ra = None

        # switch's default: rec_dispatch(...) tail-dispatch call site
        if blk.switch_default_text is not None:
            rm3 = re.search(r'rec_dispatch\(c, ([^)]+)\)', blk.switch_default_text)
            if rm3:
                live, cond_live = _split_definite_conditional(callee_live)
                call_sites.append(CallSite(
                    line_no=blk.switch_default_line, label=n,
                    target=f"rec_dispatch({rm3.group(1)})", kind="switch_default_rec_dispatch",
                    ra_const=pending_ra[0] if pending_ra else None,
                    live_regs=live, conditional_live_regs=cond_live,
                ))

    # stable ordering for readability: by (line_no)
    scratch_stores.sort(key=lambda s: s.line_no)
    call_sites.sort(key=lambda c: c.line_no)
    epilogue_restores.sort(key=lambda r: r[3])

    own_callee_saved_used = sorted({r.reg for r in prologue_spills if r.reg is not None})

    return Contract(
        name=fn.name, addr=fn.addr, path=fn.path, is_overlay=fn.is_overlay, ov_name=fn.ov_name,
        frame_size=frame_size, frame_close_sizes=frame_close_sizes,
        prologue_spills=prologue_spills, epilogue_restores=epilogue_restores,
        scratch_stores=scratch_stores, call_sites=call_sites,
        own_callee_saved_used=own_callee_saved_used,
        unreachable_block_count=unreachable_count,
    )


def _split_definite_conditional(callee_live: dict) -> tuple:
    """callee_live: reg -> set[str] (possibly containing the '<undefined on some incoming path>'
    sentinel). Returns (definite: {reg: expr}, conditional: {reg: [expr,...]})."""
    live = {}
    cond_live = {}
    for r, vals in sorted(callee_live.items()):
        vals = set(vals)
        if len(vals) == 1:
            live[r] = next(iter(vals))
        elif len(vals) > 1:
            cond_live[r] = sorted(vals)
    return live, cond_live


# --------------------------------------------------------------------------------------------------
# Op-sequence extraction (tools/port_check.py's shared token layer)
# --------------------------------------------------------------------------------------------------

def _decompound_line(raw: str) -> list:
    """Decompose a MIPS branch-delay-slot compound into its real execution order (delay insn first,
    branch marker second). Kept as the compatibility surface for tools/port_check.py's op-sequence
    extraction; the contract/CFG path uses tokenize()/_classify_stmt() instead (same regexes)."""
    cd = COMPOUND_DELAY_RE.match(raw)
    if cd:
        return [cd.group(2) + ';', f'/*branch-> {cd.group(3)}*/']
    cn = COMPOUND_NODELAY_RE.match(raw)
    if cn:
        return [f'/*branch-> {cn.group(2)}*/']
    return [raw]


@dataclass
class OpSeqCall:
    ra_const: Optional[str]   # hex string, or None if no r31 literal seen before this call
    target: Optional[str]     # func_XXXXXXXX / rec_dispatch target literal, or None if unresolved


@dataclass
class OpSequence:
    """Address-agnostic guest-visible operation sequence, for tools/port_check.py's equivalence
    check. Reuses the SAME line-level idiom regexes as parse_contract (this module's one parser —
    see docs/abi-extract.md 'do not fork a second parser'), just projected onto a coarser, renaming-
    tolerant view: frame open/close sizes, ordered call sites (ra constant + resolved target, in
    program order — NOT scoped to callee-saved liveness like Contract.call_sites), and the ordered
    sequence of memory-store WIDTHS (address-agnostic — see MEM_W_ANY_RE)."""
    frame_opens: list      # ints, program order (normally 0 or 1 entry)
    frame_closes: list     # ints, program order
    calls: list             # OpSeqCall, program order
    mem_write_widths: list  # ints (8/16/32), program order


def extract_op_sequence(body_lines) -> "OpSequence":
    """Extract an OpSequence from a raw list of C/C++ statement lines (gen body OR a native method
    body — same extraction is applied to both sides by tools/port_check.py). Uses the same
    delay-slot decompounding as the tokenizer so branch-delay-slot statements execute in the right
    order; a native C++ method has no such compounds, so decompounding is a no-op for it."""
    frame_opens, frame_closes = [], []
    calls: list = []
    widths: list = []
    pending_ra = None

    # Native ports don't necessarily use the exact `c->r[29] = c->r[29] + (uint32_t)-N;` gen idiom
    # (e.g. `c->r[29] -= N;`, or `c->r[29] += N;`), so accept both spellings here — this function is
    # intentionally MORE permissive than parse_contract's FRAME_OPEN_RE/FRAME_CLOSE_RE, which must
    # stay strict because they gate the authoritative --contract/--scaffold output.
    frame_dec_re = re.compile(r'c->r\[29\]\s*(?:-=|\+=\s*\(uint32_t\)-|=\s*c->r\[29\]\s*\+\s*\(uint32_t\)-|=\s*[A-Za-z_]\w*\s*-\s*\(?uint32_t\)?)\s*(\d+)')
    frame_inc_re = re.compile(r'c->r\[29\]\s*(?:\+=|=\s*c->r\[29\]\s*\+\s*\(uint32_t\)|=\s*[A-Za-z_]\w*\s*\+\s*\(?uint32_t\)?)\s*(\d+)')

    stream = []
    for i, raw in enumerate(body_lines):
        for stmt in _decompound_line(raw.strip()):
            stream.append((i, stmt))

    # Dead-code exclusion. Every `return;` in a recompiled gen body is UNCONDITIONAL (it is a `jr ra`;
    # conditional flow is always a branch/`goto`), so any statement after a `return;` is reachable ONLY
    # via a label (a goto target). Recompiled bodies routinely carry an unreachable trailing fall-through
    # into the NEXT sibling function after their real `return;` (e.g. `...; return; func_<next>(c); return;`
    # — the shared-epilogue / dead-sibling artifact, same class as 0x80040400). The authoritative
    # Contract/CFG path already excludes these (unreachable_block_count); this coarser linear pass must
    # match it, else a hand-rebuilt LEAF (real body has 0 calls) spuriously FAILs port_check on a phantom
    # trailing call. Track a dead region: entered by an unconditional `return;`, left by the next label.
    # The recompiler sometimes FOLDS several contiguous guest functions into ONE gen_func_ body (each
    # with its own frame open + return; e.g. gen_func_80074810 folds the 0x80074810/834/868 sfx trio).
    # Only the function reached at THIS entry is live; the rest are dead siblings. A single function opens
    # its frame exactly once at entry and never after a return, so a frame-descent seen INSIDE the post-
    # return dead region marks a folded-sibling boundary — latch `sibling` and never re-enter reachable
    # code again (the sibling's own goto/labels would otherwise spuriously reactivate). The Contract/CFG
    # path already scopes to the first function this way; this makes the linear pass agree.
    label_re = re.compile(r'^[A-Za-z_]\w*:\s*;?')
    dead = False
    sibling = False

    for _line_no, stmt in stream:
        lm = label_re.match(stmt)
        if lm and not sibling:       # a label target re-enters reachable code (unless in a folded sibling)
            dead = False
            stmt = stmt[lm.end():].strip()
            if not stmt:
                continue
        if dead:                     # unreachable statement after an unconditional return — skip
            if not sibling and frame_dec_re.search(stmt):
                sibling = True       # a new frame opened while dead = start of a folded sibling function
            continue
        # frame open (descent) / close (ascent) — checked before generic +=/-= to avoid double count
        fo = frame_dec_re.search(stmt)
        fc = frame_inc_re.search(stmt) if not fo else None
        if fo:
            frame_opens.append(int(fo.group(1)))
        elif fc:
            frame_closes.append(int(fc.group(1)))

        for w in MEM_W_ANY_RE.finditer(stmt):
            widths.append(int(w.group(1)))

        ram = RA_SET_RE.search(stmt)
        if ram:
            pending_ra = ram.group(1)

        cm = CALL_RE.search(stmt)
        rm = RECDISP_RE.search(stmt) if not cm else None
        if cm:
            calls.append(OpSeqCall(ra_const=pending_ra, target=cm.group(1)))
            pending_ra = None
        elif rm and 'default:' not in stmt:
            raw_target = rm.group(1).strip()
            lit = re.match(r'^0x([0-9A-Fa-f]+)u?$', raw_target)
            target = ('0x' + lit.group(1).upper()) if lit else None  # None = unresolved (symbolic expr)
            calls.append(OpSeqCall(ra_const=pending_ra, target=target))
            pending_ra = None

        if re.search(r'\breturn\s*;', stmt):   # unconditional return -> following stmts are dead until a label
            dead = True

    return OpSequence(frame_opens=frame_opens, frame_closes=frame_closes, calls=calls,
                       mem_write_widths=widths)


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
    if c.unreachable_block_count:
        out.append(f"unreachable_block_count = {c.unreachable_block_count}  (dead code after "
                    f"uncond goto/return with no label reusing it — excluded from analysis)")
    out.append("")

    out.append(f"## prologue spills ({len(c.prologue_spills)}), sp-relative, program order")
    for s in c.prologue_spills:
        out.append(f"  sp+{s.offset:<4} <- r{s.reg:<2}  (w{s.width})")
    if not c.prologue_spills:
        out.append("  (none)")
    out.append("")

    out.append(f"## epilogue restores ({len(c.epilogue_restores)})")
    for reg, off, width, line_no, blk_name in c.epilogue_restores:
        matched = any(s.reg == reg and s.offset == off for s in c.prologue_spills)
        tag = "" if matched else "  [!] no matching prologue spill at this offset"
        out.append(f"  [{blk_name}] r{reg:<2} <- sp+{off:<4}  (w{width}){tag}")
    if not c.epilogue_restores:
        out.append("  (none)")
    out.append("")

    out.append(f"## scratch / local sp-relative stores ({len(c.scratch_stores)}), CFG block order, "
                f"with branch-guard context")
    for s in c.scratch_stores:
        out.append(f"  [{s.label}] sp+{s.offset:<5} <- {s.source}   (w{s.width})")
        for hop in s.guard_chain:
            out.append(f"        guard: {hop}")
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
            out.append(f"       live callee-saved regs (definite, same on every reaching path): {live_str}")
        if cs.conditional_live_regs:
            for r, exprs in sorted(cs.conditional_live_regs.items()):
                out.append(f"       [!] r{r} is CONDITIONALLY live — differs by incoming path: "
                            + " | ".join(exprs))
        if not cs.live_regs and not cs.conditional_live_regs:
            out.append(f"       live callee-saved regs: (none written on any path reaching this call)")
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

    if c.scratch_stores:
        out.append("")
        out.append(f"// Per-branch scratch mirrors ({len(c.scratch_stores)}) — these are NOT part of the")
        out.append(f"// callee-save frame above; they are transient local scratch at the offsets/branch")
        out.append(f"// paths shown. Group by [block] below — each group only executes when its guard")
        out.append(f"// chain is satisfied; mirror them as locals scoped to that branch, not as fields")
        out.append(f"// on {cname} (which would falsely imply they're always live).")
        by_block = {}
        for s in c.scratch_stores:
            by_block.setdefault(s.label, []).append(s)
        for blk_name, stores in by_block.items():
            out.append(f"// -- block [{blk_name}] --")
            if stores[0].guard_chain:
                for hop in stores[0].guard_chain:
                    out.append(f"//    guard: {hop}")
            for s in stores:
                out.append(f"//    sp+{s.offset} <- {s.source}  (w{s.width})")

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
        for r, exprs in sorted(cs.conditional_live_regs.items()):
            out.append(f"// [!] r{r} is CONDITIONALLY live depending on which path reached this call:")
            for expr in exprs:
                out.append(f"//       c->r[{r}] = {expr};  // on some incoming path(s)")
            out.append(f"// mirror whichever value matches the branch actually taken on THIS call — "
                        f"do not pick one arbitrarily.")
        out.append(f"// {cs.target}(c);")
        out.append("")
    return "\n".join(out)


def render_scaffold_guestabi(c: Contract) -> str:
    """Emit a runtime/recomp/guest_abi.h-based scaffold: a `kSpills` table + `GuestFrame<...>`
    declaration, ready to paste into a new faithful port. See docs/port-framework.md."""
    out = []
    out.append(f'#include "guest_abi.h"')
    out.append(f"// Guest-stack frame for {c.name} (0x{c.addr}) — table order matches abi_extract's")
    out.append(f"// 'prologue spills' section exactly (program order). Regenerate if the gen body changes:")
    out.append(f"//   python3 tools/abi_extract.py 0x{c.addr} --scaffold --guestabi")
    if c.frame_size == 0:
        out.append(f"// NOTE: no sp descent detected in the gen body — use an empty-spill zero-size frame:")
        out.append(f"static constexpr GuestFrameSpill kSpills_{c.addr}[1] = {{}};")
        out.append(f"// GuestFrame<0, 0> frame(c, kSpills_{c.addr});   // (NumSpills must be 0; adjust decl)")
    else:
        n = len(c.prologue_spills)
        # array extent max(n,1): C++ forbids zero-length arrays, and a frame CAN legitimately have
        # zero spills (e.g. gt3's pure-scratch -24 frame) — NumSpills in the GuestFrame decl stays n.
        out.append(f"static constexpr GuestFrameSpill kSpills_{c.addr}[{max(n, 1)}] = {{")
        for s in c.prologue_spills:
            regname = "31 /*ra*/" if s.reg == 31 else str(s.reg)
            out.append(f"  {{ {regname}, {s.offset} }},")
        out.append("};")
        out.append(f"// At method entry:")
        out.append(f"//   GuestFrame<{c.frame_size}, {n}> frame(c, kSpills_{c.addr});")
    if c.scratch_stores:
        out.append("")
        out.append(f"// Per-branch scratch stores ({len(c.scratch_stores)}) — NOT covered by the spill table")
        out.append(f"// above; each group below only executes on its guarded path (mirror as branch-scoped")
        out.append(f"// writes, exactly like the gen body — the gt3/gt4 f179 bug class):")
        by_block = {}
        for s in c.scratch_stores:
            by_block.setdefault(s.label, []).append(s)
        for blk_name, stores in by_block.items():
            out.append(f"//   -- block [{blk_name}] --")
            if stores[0].guard_chain:
                for hop in stores[0].guard_chain:
                    out.append(f"//      guard: {hop}")
            for s in stores:
                out.append(f"//      sp+{s.offset} <- {s.source}  (w{s.width})")
    out.append("")
    out.append(f"// Call sites ({len(c.call_sites)}) — use guest_call/guest_dispatch so the ra constant can")
    out.append(f"// never be forgotten:")
    for i, cs in enumerate(c.call_sites):
        ra = f"0x{cs.ra_const}u" if cs.ra_const else "0x????????u /* MISSING in source — investigate */"
        if cs.kind == "direct":
            out.append(f"//   guest_call(c, {ra}, {cs.target});   // [{i}] label={cs.label}")
        else:
            target_expr = cs.target
            if target_expr.startswith("rec_dispatch("):
                target_expr = target_expr[len("rec_dispatch("):-1]
            out.append(f"//   guest_dispatch(c, {ra}, {target_expr});   // [{i}] label={cs.label}")
        for r, expr in sorted(cs.live_regs.items()):
            out.append(f"//     GuestReg<{r}>(c) = {expr};  // mirror gen's live value before this call")
        for r, exprs in sorted(cs.conditional_live_regs.items()):
            out.append(f"//     [!] r{r} CONDITIONALLY live (differs by incoming path) — mirror the value")
            for expr in exprs:
                out.append(f"//         {expr}")
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

    for s in c.scratch_stores:
        if re.search(rf'\b{s.offset}\b', text):
            oks.append(f"scratch offset {s.offset} [{s.label}] literal present")
        else:
            warnings.append(f"scratch offset {s.offset} [{s.label}] NOT found as a literal — "
                             f"is this per-branch scratch slot mirrored?")

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
        for r in cs.conditional_live_regs:
            if re.search(rf'r\[{r}\]', text):
                oks.append(f"call site {i}: r{r} is CONDITIONALLY live in source and c->r[{r}] "
                            f"appears somewhere in file (verify per-path manually)")
            else:
                warnings.append(f"call site {i} ({cs.target}): r{r} is CONDITIONALLY live in source "
                                 f"(differs by incoming branch) but c->r[{r}] never appears in native "
                                 f"file — likely missing register-faithfulness mirror")

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
    ap.add_argument('--guestabi', action='store_true',
                     help='with --scaffold, emit the runtime/recomp/guest_abi.h GuestFrame form instead '
                          'of the ad-hoc struct form')
    ap.add_argument('--audit', metavar='NATIVE_CPP', help='heuristic audit of an existing native port')
    ap.add_argument('--json', action='store_true', help='emit --contract as JSON instead of text')
    ap.add_argument('--repo', default=None, help='repo root (default: dir containing this script\'s parent)')
    args = ap.parse_args(argv)

    # Resolve the recomp corpus root. Order: --repo > $PSXPORT_GAME_ROOT > cwd (if it has generated/) >
    # this framework tool's own repo. Lets the framework porting tools run straight from a consuming game
    # repo (e.g. Tomba2Engine), whose generated/ is game-side after the framework/game repo split.
    repo = (args.repo or os.environ.get('PSXPORT_GAME_ROOT')
            or (os.getcwd() if os.path.isdir(os.path.join(os.getcwd(), 'generated')) else None)
            or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    try:
        fn = locate_function(repo, args.addr)
        contract = parse_contract(fn)

        did_something = False
        if args.scaffold:
            print(render_scaffold_guestabi(contract) if args.guestabi else render_scaffold(contract))
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
