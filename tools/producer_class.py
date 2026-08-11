#!/usr/bin/env python3
"""producer_class.py — STATIC TRIAGE: is a guest graphics producer MECHANICALLY portable?

THE QUESTION THIS ANSWERS. `docs/plans/graphics-producer-db.md` records, per graphics producer,
whether a native equivalent exists. The next question is which of the producer-less ones a TOOL can
draft and which need a human/LLM judgement call. That is decided by WHICH GTE OPERATIONS the producer
reaches:

  * NO GTE op            -> a 2D producer (HUD/panel/backdrop/sprite): packet fields come from direct
                            struct reads, so the native port reads the same fields and pushes a quad.
  * only the PROJECTION/GEOMETRY subset (RTPS/RTPT/NCLIP/AVSZ3/AVSZ4/OP/SQR, and MVMVA selecting the
                            ROTATION matrix) -> a rigid transform+project. The native float camera and
                            projection already exist, so the port reads the node's own pose, calls the
                            shared projection, and emits the same faces. Templatable.
  * per-vertex LIGHTING/COLOUR ops (NCDS/NCCS/NCDT/DPCS/DPCT/INTPL/CDP/DCPL/GPF/GPL/CC/...) -> the
                            native equivalent is a DESIGN DECISION (what is this with float colour and
                            a real depth buffer?), not a transcription.

WHY THE WHOLE-SUBTREE ANSWER IS THE WRONG AXIS, WHICH IS THE POINT OF THIS TOOL.
Measured on Tomba!2's effect-mesh family: 18 of 20 controllers came back "needs judgement" — with the
IDENTICAL op signature, because the lighting ops are not in the controllers at all. They live in two
SHARED callees: the writer `0x80027768` (DPCS/DPCT depth cue, 21 call sites) and the math helper
`0x80085480` (GPF, 39 call sites). A shared callee is ported ONCE for all its callers, so charging its
ops to each caller turns "two judgement calls" into "eighteen" and hides the fact that one of the two
was ALREADY resolved (game/render/fx_plume.cpp: the guest programs the cue to the identity, so it is a
no-op in this family).

So every verdict is reported on TWO axes:
  * `subtree_class` — every op inlined. Honest, and the right answer for "what does this code do".
  * `own_class`     — the same walk with SHARED and ALREADY-PORTED callees cut at the edge. This is
                      the ACTIONABLE axis: "given the shared dependencies are handled, is THIS
                      producer mechanical?" Cut deps are reported as `blocked_on`, each of which is
                      one judgement call amortised over all its callers.
"Shared" is decided by CORPUS FAN-IN (call sites across all of generated/), not a hand-maintained
list, so it cannot silently rot. `--frontier` adds addresses already ported natively.

SOUNDNESS, AND WHY THE NEGATIVE IS THE HARD DIRECTION. 14.3% of this substrate's `rec_dispatch`
targets are computed at runtime (`_tgt` / `c->r[2]`), so the static call graph has holes. The two
directions are NOT symmetric:

  * "REACHES GTE" is sound even with holes — a witness was found, and an edge we could not follow
    cannot un-find it.
  * "REACHES NO GTE" is sound ONLY if the walked subtree had ZERO unresolved edges. With even one
    unresolved edge the honest answer is BLIND, not 2D.

A subtree with unresolved edges and no GTE witness is therefore reported as `blind`, WITH the count
and the call sites, and is never silently promoted to the draftable class: that would hand a generator
a producer whose projection it cannot see. (`CLAUDE.md`: "a diagnostic that can print nothing is
lying" — the negative here carries its denominator and names its blind spots.)

TWO MORE WAYS THE NAIVE VERSION OF THIS TOOL LIES, both handled:
  1. The recompiler FOLDS contiguous guest functions: a body's real code ends at a bare `return;` and
     the NEXT function's code follows as unreachable trailing text before the closing brace
     (abi_extract.py / docs/abi-extract.md). Scanning raw body text therefore attributes the NEXT
     function's GTE ops to this one. Every scan here is restricted to blocks REACHABLE from the entry
     via abi_extract's own CFG — this file does not fork a second generated/*.c parser.
  2. Misdecoded data-as-code emits `gte_op` words whose function field is not a real GTE opcode. Those
     are counted and reported as `invalid_gte_ops`, never silently dropped nor treated as lighting.

USAGE
    python3 tools/producer_class.py --repo <game-repo> classify 0x8002BC9C [0x800288AC ...]
    python3 tools/producer_class.py --repo <game-repo> classify --file addrs.txt [--json out.json]
    python3 tools/producer_class.py --repo <game-repo> classify --file a.txt --frontier ported.txt
    python3 tools/producer_class.py --repo <game-repo> selftest

`selftest` is mandatory maintenance, not decoration: it runs the classifier against a producer known
to project (must come back GTE-reaching) AND one known not to (must come back 2D). A discriminator
validated against one class only is not known to discriminate (CLAUDE.md).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi_extract as abi  # reuse the generated/*.c emission dialect + CFG. Do not fork a parser.


# --------------------------------------------------------------------------------------------------
# GTE opcode decode
# --------------------------------------------------------------------------------------------------

# COP2 instruction, CO=1: 0x4A000000 | <25 bits>. Function field = bits 5..0.
GTE_OP_NAMES = {
    0x01: 'RTPS',  0x06: 'NCLIP', 0x0C: 'OP',    0x10: 'DPCS',  0x11: 'INTPL', 0x12: 'MVMVA',
    0x13: 'NCDS',  0x14: 'CDP',   0x16: 'NCDT',  0x1B: 'NCCS',  0x1C: 'CC',    0x1E: 'NCS',
    0x20: 'NCT',   0x28: 'SQR',   0x29: 'DCPL',  0x2A: 'DPCT',  0x2D: 'AVSZ3', 0x2E: 'AVSZ4',
    0x30: 'RTPT',  0x3D: 'GPF',   0x3E: 'GPL',   0x3F: 'NCCT',
}

# Ops whose native replacement is the float camera + projection this port ALREADY has. A producer
# reaching only these is a transform-and-project, which is templatable.
GEOMETRY_OPS = {'RTPS', 'RTPT', 'NCLIP', 'AVSZ3', 'AVSZ4', 'OP', 'SQR'}

# MVMVA is generic matrix*vector — geometry or lighting depending on its MATRIX SELECT bits, which are
# encoded in the instruction word. Decoded, not assumed: mx=0 rotation is geometry, mx=1 light and
# mx=2 colour are per-vertex shading.
MVMVA_MX = {0: 'rot', 1: 'light', 2: 'colour', 3: 'reserved'}


def decode_gte_op(word: int) -> dict:
    """Decode a `gte_op(c, <word>)` literal into {'name','func','valid','is_geometry', ...}."""
    func = word & 0x3F
    name = GTE_OP_NAMES.get(func)
    out = {'word': word, 'func': func, 'name': name or f'?{func:02X}', 'valid': name is not None}
    if name == 'MVMVA':
        mx = (word >> 17) & 3
        out['mvmva_mx'] = MVMVA_MX[mx]
        out['mvmva_v'] = (word >> 15) & 3
        out['mvmva_cv'] = (word >> 13) & 3
        out['is_geometry'] = (mx == 0)
    else:
        out['is_geometry'] = name in GEOMETRY_OPS
    return out


def op_key(op: dict) -> str:
    """Histogram key: MVMVA is split by matrix select, since that is what decides its class."""
    return op['name'] if op['name'] != 'MVMVA' else f"MVMVA.{op.get('mvmva_mx')}"


# --------------------------------------------------------------------------------------------------
# Bulk index of generated/ + corpus fan-in
# --------------------------------------------------------------------------------------------------

@dataclass
class BodyRef:
    name: str
    addr: str
    path: str
    start_line: int
    end_line: int
    body: list


@dataclass
class Corpus:
    bodies: dict                      # ADDR -> BodyRef
    fanin: dict                       # ADDR -> number of call sites across all of generated/
    files: int = 0


def index_generated(repo: str) -> Corpus:
    """Index every gen body under <repo>/generated/*.c, and count each address's call sites.

    Refuses loudly on a missing/empty corpus: a classifier reporting "no GTE found" over zero parsed
    functions is precisely the failure this file exists to prevent, so that case exits non-zero
    saying it searched NOTHING rather than returning a clean empty answer.
    """
    gen_dir = os.path.join(repo, 'generated')
    if not os.path.isdir(gen_dir):
        raise abi.AbiParseError(
            f"generated/ not found under {repo!r} — NOTHING WAS SEARCHED. Build the recomp corpus "
            f"first (./run.sh, or tools/ensure_recomp.py); do not read this as 'no producers found'.")
    files = sorted(glob.glob(os.path.join(gen_dir, '*.c')))
    if not files:
        raise abi.AbiParseError(f"no *.c under {gen_dir} — NOTHING WAS SEARCHED (see above).")

    bodies: dict = {}
    fanin: dict = {}
    for path in files:
        with open(path, 'r', errors='replace') as f:
            lines = f.readlines()
        text = ''.join(lines)
        for m in abi.CALL_RE.finditer(text):
            a = m.group(2).upper()
            fanin[a] = fanin.get(a, 0) + 1
        for m in abi.RECDISP_RE.finditer(text):
            t = m.group(1).strip()
            if t.startswith('0x'):
                a = f"{int(t.rstrip('u'), 16) & 0xFFFFFFFF:08X}"
                fanin[a] = fanin.get(a, 0) + 1
        i, n = 0, len(lines)
        while i < n:
            m = abi.FUNC_OPEN_RE.match(lines[i].rstrip('\n'))
            if not m:
                i += 1
                continue
            name, faddr = m.group(1), m.group(2).upper()
            j = i + 1
            while j < n and lines[j].rstrip('\n') != '}':
                j += 1
            if j >= n:
                i += 1
                continue
            bodies[faddr] = BodyRef(name=name, addr=faddr, path=path, start_line=i + 1,
                                    end_line=j + 1, body=[l.rstrip('\n') for l in lines[i + 1:j]])
            i = j + 1
    if not bodies:
        raise abi.AbiParseError(
            f"parsed {len(files)} file(s) under {gen_dir} and found ZERO gen bodies — the emission "
            f"dialect changed and abi_extract.FUNC_OPEN_RE no longer matches. That is a BROKEN "
            f"INSTRUMENT, not an empty corpus.")
    return Corpus(bodies=bodies, fanin=fanin, files=len(files))


# --------------------------------------------------------------------------------------------------
# Per-body facts, restricted to blocks REACHABLE from the entry
# --------------------------------------------------------------------------------------------------

@dataclass
class BodyFacts:
    addr: str
    name: str
    gte_ops: list = field(default_factory=list)      # decoded dicts, reachable blocks only
    gte_helpers: list = field(default_factory=list)   # gte_write_ctrl/read_data/... names seen
    callees: set = field(default_factory=set)         # resolved ADDR strings
    unresolved: list = field(default_factory=list)     # (kind, text) edges we could not follow
    stores: int = 0                                    # mem_w* count — packet-building volume
    reachable_blocks: int = 0
    total_blocks: int = 0
    parse_error: str = ""


GTE_HELPER_RE = re.compile(r'\bgte_([a-z_0-9]+)\s*\(')
GTE_OP_LIT_RE = re.compile(r'\bgte_op\(c,\s*(0x[0-9A-Fa-f]+)u?\s*\)')
GTE_OP_ANY_RE = re.compile(r'\bgte_op\(c,')
MEM_W_RE = re.compile(r'c->mem_w(?:8|16|32)\s*\(')

_facts_cache: dict = {}


def analyse_body(ref: BodyRef) -> BodyFacts:
    """Extract GTE/callee/store facts from ONLY the statements reachable from the entry block."""
    if ref.addr in _facts_cache:
        return _facts_cache[ref.addr]
    f = BodyFacts(addr=ref.addr, name=ref.name)
    try:
        toks = abi.tokenize(ref.body)
        blocks, order, entry = abi.build_cfg(toks)
        reach = abi._reachable_blocks(blocks, order, entry)
    except Exception as e:  # a body we cannot CFG is a blind spot, and must SAY so
        f.parse_error = f"{type(e).__name__}: {e}"
        f.unresolved.append(('cfg-parse-failed', f.parse_error))
        _facts_cache[ref.addr] = f
        return f
    f.total_blocks = len(order)
    f.reachable_blocks = len(reach)

    saw_gte_op_call = False
    for bname in reach:
        for tok in blocks[bname].stmts:
            s = tok.text if hasattr(tok, 'text') else str(tok)
            for m in GTE_OP_LIT_RE.finditer(s):
                f.gte_ops.append(decode_gte_op(int(m.group(1), 16)))
            if GTE_OP_ANY_RE.search(s):
                saw_gte_op_call = True
            for m in GTE_HELPER_RE.finditer(s):
                h = m.group(1)
                if h != 'op':
                    f.gte_helpers.append(h)
            for m in abi.CALL_RE.finditer(s):
                f.callees.add(m.group(2).upper())
            for m in abi.RECDISP_RE.finditer(s):
                tgt = m.group(1).strip()
                if tgt.startswith('0x'):
                    f.callees.add(f"{int(tgt.rstrip('u'), 16) & 0xFFFFFFFF:08X}")
                else:
                    f.unresolved.append(('rec_dispatch-computed', tgt))
            f.stores += len(MEM_W_RE.findall(s))
    # A gte_op whose opcode we could not read (register-built) is a blind spot, not an absence.
    if saw_gte_op_call and not f.gte_ops:
        f.unresolved.append(('gte_op-nonliteral', 'gte_op called with a non-literal opcode'))
    _facts_cache[ref.addr] = f
    return f


# --------------------------------------------------------------------------------------------------
# Subtree walk + classification
# --------------------------------------------------------------------------------------------------

CLASS_2D = 'portable-2d'
CLASS_RIGID = 'portable-rigid-mesh'
# Zero GTE ops of its OWN, but a cut shared dependency carries the GEOMETRY. This is NOT the 2D class
# and the distinction is load-bearing: a 2D verdict tells a generator to push a flat quad, whereas
# these controllers only marshal node state and hand it to the shared writer, so the draft is a WRITER
# INVOCATION. Measured on Tomba!2: 5 of the 20 effect-mesh controllers are this shape.
CLASS_DELEGATING = 'portable-delegates-to-shared-writer'
CLASS_LIGHTING = 'needs-judgement-lighting'
CLASS_BLIND = 'blind-cannot-conclude'
CLASS_MISSING = 'no-gen-body'

# A callee reached by at least this many call sites across generated/ is SHARED infrastructure: it is
# ported once for every caller, so charging its ops to each caller inflates the judgement count. Set
# from the measured Tomba!2 family (writer 21 sites, math helper 39) well above a family's own size.
DEFAULT_SHARED_FANIN = 10


@dataclass
class SharedDep:
    addr: str
    fanin: int
    ops: dict
    why: str          # 'fan-in' | 'frontier'


@dataclass
class Verdict:
    root: str
    subtree_class: str = ''
    own_class: str = ''
    reason: str = ''
    own_reason: str = ''
    nodes_walked: int = 0
    max_depth: int = 0
    subtree_op_hist: dict = field(default_factory=dict)
    own_op_hist: dict = field(default_factory=dict)
    own_lighting_ops: list = field(default_factory=list)
    subtree_lighting_ops: list = field(default_factory=list)
    blocked_on: list = field(default_factory=list)     # SharedDep, each = ONE amortised judgement
    invalid_gte_ops: dict = field(default_factory=dict)
    own_stores: int = 0
    subtree_stores: int = 0
    unresolved: list = field(default_factory=list)     # (fn_addr, kind, text)
    missing_callees: list = field(default_factory=list)
    witness: str = ''
    cut_geometry: list = field(default_factory=list)
    parse_errors: list = field(default_factory=list)

    def to_dict(self) -> dict:
        d = {k: v for k, v in self.__dict__.items()}
        d['blocked_on'] = [b.__dict__ for b in self.blocked_on]
        d['unresolved_count'] = len(self.unresolved)
        d['unresolved'] = self.unresolved[:40]
        d['missing_callees'] = self.missing_callees[:40]
        return d


# Literal dispatch targets that are BIOS/HLE vectors rather than recompiled guest functions — platform
# entry points the framework services natively, so a missing gen body for one is not a blind spot in
# the producer's geometry. Kept deliberately narrow.
def is_platform_vector(addr: str) -> bool:
    a = int(addr, 16)
    return (a & 0xFFFF0000) == 0x8FFC0000 or 0x80000000 <= a <= 0x800000FF


def _norm(addr: str) -> str:
    a = addr.strip().upper()
    if a.startswith('0X'):
        a = a[2:]
    return a.zfill(8)


def classify(corpus: Corpus, root: str, frontier: set = frozenset(),
             shared_fanin: int = DEFAULT_SHARED_FANIN, max_nodes: int = 20000) -> Verdict:
    """Walk root's call subtree and classify it on both the subtree and own-ops axes."""
    root = _norm(root)
    if root not in corpus.bodies:
        v = Verdict(root=root, subtree_class=CLASS_MISSING, own_class=CLASS_MISSING)
        v.reason = (f"no gen body for 0x{root} in generated/ — NOT classified. Either the address is "
                    f"wrong or it lives in an overlay this corpus does not contain "
                    f"({len(corpus.bodies)} bodies searched across {corpus.files} files).")
        v.own_reason = v.reason
        return v

    v = Verdict(root=root)
    per_fn: dict = {}          # ADDR -> {'ops': {...}, 'stores': n, 'depth': d}
    cut: dict = {}             # ADDR -> SharedDep for edges we stopped at
    seen = {root}
    stack = [(root, 0)]
    while stack:
        addr, depth = stack.pop()
        v.nodes_walked += 1
        v.max_depth = max(v.max_depth, depth)
        if v.nodes_walked > max_nodes:
            v.unresolved.append((addr, 'walk-truncated',
                                 f'subtree exceeded {max_nodes} nodes; the walk STOPPED here — this is '
                                 f'not "nothing further found"'))
            break
        f = analyse_body(corpus.bodies[addr])
        if f.parse_error:
            v.parse_errors.append(f"{addr}: {f.parse_error}")
        ops: dict = {}
        for op in f.gte_ops:
            if not op['valid']:
                v.invalid_gte_ops[op['name']] = v.invalid_gte_ops.get(op['name'], 0) + 1
                continue
            ops[op_key(op)] = ops.get(op_key(op), 0) + 1
        per_fn[addr] = {'ops': ops, 'stores': f.stores, 'depth': depth,
                        'geometry': {k: True for k in ops
                                     if k in GEOMETRY_OPS or k == 'MVMVA.rot'}}
        for kind, text in f.unresolved:
            v.unresolved.append((addr, kind, text))
        for cal in sorted(f.callees):
            if cal in seen:
                continue
            if cal not in corpus.bodies:
                if not is_platform_vector(cal):
                    v.missing_callees.append(cal)
                continue
            # Cut SHARED / already-PORTED callees: they are one judgement amortised over all callers,
            # so their ops must not be charged to this root's OWN axis. The subtree axis still counts
            # them, which is why both are reported.
            fin = corpus.fanin.get(cal, 0)
            if cal != root and (cal in frontier or fin >= shared_fanin):
                if cal not in cut:
                    cf = analyse_body(corpus.bodies[cal])
                    cops: dict = {}
                    for op in cf.gte_ops:
                        if op['valid']:
                            cops[op_key(op)] = cops.get(op_key(op), 0) + 1
                    cut[cal] = SharedDep(addr=cal, fanin=fin, ops=cops,
                                         why='frontier' if cal in frontier else 'fan-in')
                seen.add(cal)
                stack.append((cal, depth + 1))   # still WALKED for the subtree axis
                continue
            seen.add(cal)
            stack.append((cal, depth + 1))

    # ---- accumulate the two axes ----------------------------------------------------------------
    for addr, rec in per_fn.items():
        for k, n in rec['ops'].items():
            v.subtree_op_hist[k] = v.subtree_op_hist.get(k, 0) + n
        v.subtree_stores += rec['stores']
        if addr in cut:
            continue
        for k, n in rec['ops'].items():
            v.own_op_hist[k] = v.own_op_hist.get(k, 0) + n
        v.own_stores += rec['stores']
    v.blocked_on = [cut[a] for a in sorted(cut) if any(
        k not in GEOMETRY_OPS and k != 'MVMVA.rot' for k in cut[a].ops)]

    def _lighting(hist: dict) -> list:
        return sorted(k for k in hist if k not in GEOMETRY_OPS and k != 'MVMVA.rot')

    v.subtree_lighting_ops = _lighting(v.subtree_op_hist)
    v.own_lighting_ops = _lighting(v.own_op_hist)
    for addr, rec in per_fn.items():
        if rec['ops'] and not v.witness:
            v.witness = f"0x{addr} ({corpus.bodies[addr].name})"

    blind = len(v.unresolved) + len(v.missing_callees)
    # Did any CUT dependency carry geometry? Then a root with no ops of its own is a delegating
    # controller, not a 2D producer (see CLASS_DELEGATING).
    v.cut_geometry = sorted({k for a in cut for k in cut[a].ops
                             if k in GEOMETRY_OPS or k == 'MVMVA.rot'})
    v.subtree_class, v.reason = _verdict(v.subtree_op_hist, v.subtree_lighting_ops, blind, v,
                                         axis='subtree')
    v.own_class, v.own_reason = _verdict(v.own_op_hist, v.own_lighting_ops, blind, v, axis='own')
    return v


def _verdict(hist: dict, lighting: list, blind: int, v: Verdict, axis: str) -> tuple:
    """Classify one axis. The negative direction carries its denominator and its blind spots."""
    if hist:
        if not lighting:
            note = ''
            if blind:
                note = (f" NOTE: {blind} unresolved edge(s) — this POSITIVE stands (the witness cannot "
                        f"be un-found), but an unwalked edge could add lighting ops, so the draft must "
                        f"be pixel-gated against the gte leg.")
            extra = ''
            if axis == 'own' and v.blocked_on:
                extra = (f" GIVEN {len(v.blocked_on)} shared dependency/ies are handled: "
                         f"{', '.join('0x' + b.addr for b in v.blocked_on)}.")
            return CLASS_RIGID, (
                f"reaches ONLY projection/geometry GTE ops ({_hist(hist)}); witness {v.witness}. The "
                f"native float camera + projection replace these directly, so a template draft is "
                f"possible.{extra}{note}")
        return CLASS_LIGHTING, (
            f"reaches per-vertex lighting/colour ops ({', '.join(lighting)}) alongside geometry "
            f"({_hist(hist)}). What those become natively (float colour, real depth buffer) is a "
            f"design decision, not a transcription.")
    if axis == 'own' and v.cut_geometry:
        return CLASS_DELEGATING, (
            f"no GTE op of its OWN, but the cut shared dependency/ies carry the geometry "
            f"({', '.join(v.cut_geometry)}). This is a thin controller: it marshals node state and "
            f"hands it to the shared writer, so the native draft is a WRITER INVOCATION, not a 2D "
            f"quad. {v.own_stores} own store(s) vs {v.subtree_stores} across the subtree."
            + (f" Shared dep(s) still needing one judgement each: "
               f"{', '.join('0x' + b.addr for b in v.blocked_on)}." if v.blocked_on else ""))
    if blind:
        return CLASS_BLIND, (
            f"NO GTE op found in {v.nodes_walked} walked function(s) — BUT the walk had {blind} "
            f"unresolved edge(s) ({len(v.unresolved)} computed/unparsed, {len(v.missing_callees)} "
            f"literal target(s) with no gen body), so 'no projection' is NOT established. This is NOT "
            f"the 2D class. Resolve the edges (seed the recompiler, or census the target at runtime) "
            f"before drafting anything.")
    return CLASS_2D, (
        f"walked {v.nodes_walked} function(s) to depth {v.max_depth} with ZERO unresolved edges and "
        f"ZERO GTE ops: no projection anywhere in the subtree. A 2D producer — packet fields come from "
        f"direct struct reads, so the native port reads the same fields and pushes a quad. "
        f"{v.own_stores} guest store(s).")


def _hist(h: dict) -> str:
    return ', '.join(f"{k}x{n}" for k, n in sorted(h.items(), key=lambda kv: -kv[1]))


# --------------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------------

def _load_addr_file(path: str) -> list:
    out = []
    with open(path) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if line:
                out.append(line)
    return out


def cmd_classify(args) -> int:
    corpus = index_generated(args.repo)
    addrs = list(args.addrs)
    if args.file:
        addrs += _load_addr_file(args.file)
    frontier = set(_norm(a) for a in _load_addr_file(args.frontier)) if args.frontier else set()
    if not addrs:
        print("no addresses given (pass hex addresses or --file) — NOTHING WAS CLASSIFIED",
              file=sys.stderr)
        return 2
    print(f"# corpus: {len(corpus.bodies)} gen bodies, {corpus.files} files under "
          f"{os.path.join(args.repo, 'generated')}")
    print(f"# shared-callee threshold: fan-in >= {args.shared_fanin} call sites"
          f"{f'; frontier: {len(frontier)} already-ported address(es)' if frontier else ''}")
    verdicts = []
    for a in addrs:
        v = classify(corpus, a, frontier=frontier, shared_fanin=args.shared_fanin)
        verdicts.append(v)
        same = v.own_class == v.subtree_class
        print(f"\n0x{v.root}  own={v.own_class}  subtree={v.subtree_class}"
              f"{'' if same else '   <-- AXES DISAGREE: shared deps carry the lighting'}")
        print(f"  own:     {v.own_reason}")
        if not same:
            print(f"  subtree: {v.reason}")
        if v.own_op_hist:
            print(f"  own gte ops:     {_hist(v.own_op_hist)}")
        if v.blocked_on:
            print(f"  blocked on {len(v.blocked_on)} SHARED dep(s) — one judgement each, amortised:")
            for b in v.blocked_on:
                print(f"      0x{b.addr}  fan-in {b.fanin} call sites ({b.why})  ops: {_hist(b.ops)}")
        if v.invalid_gte_ops:
            print(f"  INVALID gte_op words (data-as-code?): {_hist(v.invalid_gte_ops)}")
        print(f"  walked:  {v.nodes_walked} fn, depth {v.max_depth}, "
              f"{v.own_stores} own / {v.subtree_stores} subtree guest stores")
        if v.unresolved:
            print(f"  UNRESOLVED: {len(v.unresolved)} edge(s)")
            for fn, kind, text in v.unresolved[:4]:
                print(f"              0x{fn}: {kind}: {text}")
            if len(v.unresolved) > 4:
                print(f"              … {len(v.unresolved) - 4} more")
        if v.missing_callees:
            print(f"  NO GEN BODY: {len(v.missing_callees)} literal target(s): "
                  f"{', '.join('0x' + m for m in v.missing_callees[:6])}"
                  f"{' …' if len(v.missing_callees) > 6 else ''}")
        if v.parse_errors:
            print(f"  PARSE ERRORS: {len(v.parse_errors)}")
            for pe in v.parse_errors[:3]:
                print(f"                {pe}")

    print(f"\n# ---- summary over {len(verdicts)} address(es) ----")
    for label, key in (('own (shared deps cut)', 'own_class'), ('subtree (all inlined)', 'subtree_class')):
        hist: dict = {}
        for v in verdicts:
            c = getattr(v, key)
            hist[c] = hist.get(c, 0) + 1
        draftable = (hist.get(CLASS_2D, 0) + hist.get(CLASS_RIGID, 0)
                     + hist.get(CLASS_DELEGATING, 0))
        parts = ', '.join(f"{k}={hist[k]}" for k in sorted(hist))
        print(f"#   {label:24s} {parts}")
        print(f"#   {'':24s} => MECHANICALLY DRAFTABLE: {draftable}/{len(verdicts)}")
    allblk: dict = {}
    for v in verdicts:
        for b in v.blocked_on:
            allblk[b.addr] = b
    if allblk:
        print(f"#   DISTINCT shared dependencies needing judgement ONCE: {len(allblk)}")
        for a in sorted(allblk):
            print(f"#      0x{a}  fan-in {allblk[a].fanin}  ops: {_hist(allblk[a].ops)}")
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'corpus_bodies': len(corpus.bodies), 'corpus_files': corpus.files,
                       'shared_fanin': args.shared_fanin,
                       'verdicts': [v.to_dict() for v in verdicts]}, f, indent=2)
        print(f"# wrote {args.json}")
    return 0


def cmd_selftest(args) -> int:
    """Run the discriminator against BOTH classes. A tool validated on one is not a discriminator.

    Cases come from the GAME (they are game addresses), via
    <repo>/docs/producers/classify-selftest.json:
        {"projecting": ["8002BC9C", ...], "flat2d": ["800xxxxx", ...]}
    A missing file exits 2 = INAPPLICABLE, never 0 = pass: this game has supplied no ground truth.
    """
    spec_path = os.path.join(args.repo, 'docs', 'producers', 'classify-selftest.json')
    if not os.path.isfile(spec_path):
        print(f"SELFTEST INAPPLICABLE: {spec_path} does not exist, so the classifier was validated "
              f"against NOTHING. Supply known-projecting and known-flat producer addresses for this "
              f"game before trusting any verdict.", file=sys.stderr)
        return 2
    with open(spec_path) as f:
        spec = json.load(f)
    if not spec.get('projecting') or not spec.get('flat2d'):
        print(f"SELFTEST ONE-SIDED: {len(spec.get('projecting', []))} projecting / "
              f"{len(spec.get('flat2d', []))} flat case(s). A discriminator run against one class only "
              f"is not known to discriminate — supply both.", file=sys.stderr)
        return 2
    corpus = index_generated(args.repo)
    fails = checked = 0
    for addr in spec['projecting']:
        v = classify(corpus, addr)
        checked += 1
        ok = v.subtree_class in (CLASS_RIGID, CLASS_LIGHTING)
        print(f"[{'ok  ' if ok else 'FAIL'}] 0x{v.root} expected GTE-reaching, got {v.subtree_class}")
        fails += 0 if ok else 1
    for addr in spec['flat2d']:
        v = classify(corpus, addr)
        checked += 1
        ok = v.subtree_class == CLASS_2D
        print(f"[{'ok  ' if ok else 'FAIL'}] 0x{v.root} expected {CLASS_2D}, got {v.subtree_class}")
        fails += 0 if ok else 1
    print(f"\n{checked - fails}/{checked} case(s) correct, in BOTH directions")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default='.', help='game repo root (holds generated/)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('classify', help='classify one or more producer addresses')
    c.add_argument('addrs', nargs='*')
    c.add_argument('--file', help='file of hex addresses, one per line (# comments ok)')
    c.add_argument('--frontier', help='file of addresses already ported natively (cut as shared)')
    c.add_argument('--shared-fanin', type=int, default=DEFAULT_SHARED_FANIN,
                   help=f'call-site count at which a callee counts as shared (default '
                        f'{DEFAULT_SHARED_FANIN})')
    c.add_argument('--json', help='write machine-readable verdicts here')
    c.set_defaults(fn=cmd_classify)
    s = sub.add_parser('selftest', help='validate the discriminator against BOTH classes')
    s.set_defaults(fn=cmd_selftest)
    args = ap.parse_args()
    try:
        return args.fn(args)
    except abi.AbiParseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
