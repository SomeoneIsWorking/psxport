#!/usr/bin/env python3
"""Static recompiler emitter: MAIN.EXE -> C (S1).

One C function per game function (boundaries from the Ghidra function list), all operating
on a shared `R3000` state + flat memory (see runtime/recomp/r3000.h). Generated code is
SACROSANCT — never hand-edit; fix this emitter and regenerate.

Control flow:
  * intra-function branch/j  -> `goto L_<addr>`  (labels emitted at collected targets)
  * direct jal/j to a known function -> direct C call
  * jal/j to a non-recompiled address -> rec_dispatch(c, addr)  (HLE/overlay routing)
  * jr ra -> return;   jr/jalr <reg> -> rec_dispatch(c, reg)    (computed)
  * every branch/jump executes its delay-slot instruction first (emitted inline)

Known faithful-first simplifications (verified later by the diff harness, S4), documented
not hidden:
  * load-delay not modeled (compiler-scheduled MIPS code doesn't depend on it);
  * add/addi overflow traps treated as wrapping (== addu/addiu);
  * computed `jr` (switch jump tables) routed through rec_dispatch — in-function jump-table
    recovery is a later pass.

Usage: python3 tools/recomp/emit.py <MAIN.EXE> <out.c> [--limit N]
"""
import os, sys, re
sys.path.insert(0, os.path.dirname(__file__))
import psexe
import decode as D
from decode import decode

# Recomp version stamp — BUMP this whenever the recompiler logic or the seed set changes in a way that
# must invalidate every machine's existing generated/ output. tools/ensure_recomp.py folds it into the
# recomp identity and re-emits when the stamp on disk (generated/.recomp_version) differs, so a stale
# generated/ on another box (which an input-content hash alone failed to catch — a box can build a
# self-consistent-but-outdated set) is forced to regenerate. Date + a per-day counter; keep it terse.
RECOMP_VERSION = "2026-07-22.2"

R = lambda n: f"c->r[{n}]"


def wr(n, expr):
    """Assignment to GPR n, discarding writes to r0."""
    return "" if n == 0 else f"{R(n)} = {expr};"


def simm_lit(ins):
    return f"(uint32_t){ins.simm}"


def addr_expr(ins):
    return f"({R(ins.rs)} + {simm_lit(ins)})"


def emit_simple(ins):
    """C for a non-control instruction (returns a statement string, '' for nop)."""
    k, o = ins.kind, ins.op
    if k == D.NOP:
        return ""
    if k == D.LUI:
        return wr(ins.rt, f"(uint32_t){ins.imm}u << 16")
    if k == D.ALU_RRR:
        a, b = R(ins.rs), R(ins.rt)
        e = {
            "add": f"{a} + {b}", "addu": f"{a} + {b}",
            "sub": f"{a} - {b}", "subu": f"{a} - {b}",
            "and": f"{a} & {b}", "or": f"{a} | {b}",
            "xor": f"{a} ^ {b}", "nor": f"~({a} | {b})",
            "slt": f"(uint32_t)((int32_t){a} < (int32_t){b})",
            "sltu": f"(uint32_t)({a} < {b})",
        }[o]
        return wr(ins.rd, e)
    if k == D.ALU_RRI:
        a = R(ins.rs)
        if o in ("addi", "addiu"):
            e = f"{a} + {simm_lit(ins)}"
        elif o == "slti":
            e = f"(uint32_t)((int32_t){a} < {ins.simm})"
        elif o == "sltiu":
            e = f"(uint32_t)({a} < {simm_lit(ins)})"
        elif o == "andi":
            e = f"{a} & {ins.imm}u"
        elif o == "ori":
            e = f"{a} | {ins.imm}u"
        elif o == "xori":
            e = f"{a} ^ {ins.imm}u"
        else:
            raise ValueError(o)
        return wr(ins.rt, e)
    if k == D.SHIFT_I:
        t = R(ins.rt)
        e = {"sll": f"{t} << {ins.shamt}", "srl": f"{t} >> {ins.shamt}",
             "sra": f"(uint32_t)((int32_t){t} >> {ins.shamt})"}[o]
        return wr(ins.rd, e)
    if k == D.SHIFT_V:
        t, s = R(ins.rt), R(ins.rs)
        e = {"sllv": f"{t} << ({s} & 31)", "srlv": f"{t} >> ({s} & 31)",
             "srav": f"(uint32_t)((int32_t){t} >> ({s} & 31))"}[o]
        return wr(ins.rd, e)
    if k == D.MULDIV:
        a, b = R(ins.rs), R(ins.rt)
        if o == "mult":
            return (f"{{ int64_t _p = (int64_t)(int32_t){a} * (int64_t)(int32_t){b}; "
                    f"c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32); }}")
        if o == "multu":
            return (f"{{ uint64_t _p = (uint64_t){a} * (uint64_t){b}; "
                    f"c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32); }}")
        if o == "div":
            return f"cpu_div(c, {a}, {b});"
        if o == "divu":
            return f"cpu_divu(c, {a}, {b});"
    if k == D.HILO:
        if o == "mfhi":
            return wr(ins.rd, "c->hi")
        if o == "mflo":
            return wr(ins.rd, "c->lo")
        if o == "mthi":
            return f"c->hi = {R(ins.rs)};"
        if o == "mtlo":
            return f"c->lo = {R(ins.rs)};"
    if k == D.LOAD:
        a = addr_expr(ins)
        if o == "lb":
            return wr(ins.rt, f"(uint32_t)(int8_t)c->mem_r8({a})")
        if o == "lbu":
            return wr(ins.rt, f"(uint32_t)c->mem_r8({a})")
        if o == "lh":
            return wr(ins.rt, f"(uint32_t)(int16_t)c->mem_r16({a})")
        if o == "lhu":
            return wr(ins.rt, f"(uint32_t)c->mem_r16({a})")
        if o == "lw":
            return wr(ins.rt, f"c->mem_r32({a})")
        if o == "lwl":
            return wr(ins.rt, f"c->mem_lwl({R(ins.rt)}, {a})")
        if o == "lwr":
            return wr(ins.rt, f"c->mem_lwr({R(ins.rt)}, {a})")
    if k == D.STORE:
        a = addr_expr(ins)
        if o == "sb":
            return f"c->mem_w8({a}, (uint8_t){R(ins.rt)});"
        if o == "sh":
            return f"c->mem_w16({a}, (uint16_t){R(ins.rt)});"
        if o == "sw":
            return f"c->mem_w32({a}, {R(ins.rt)});"
        if o == "swl":
            return f"c->mem_swl({a}, {R(ins.rt)});"
        if o == "swr":
            return f"c->mem_swr({a}, {R(ins.rt)});"
    if k == D.GTE_MOVE:
        if o == "mfc2":
            return wr(ins.rt, f"gte_read_data({ins.rd})")
        if o == "cfc2":
            return wr(ins.rt, f"gte_read_ctrl({ins.rd})")
        if o == "mtc2":
            return f"gte_write_data({ins.rd}, {R(ins.rt)});"
        if o == "ctc2":
            return f"gte_write_ctrl({ins.rd}, {R(ins.rt)});"
    if k == D.GTE_OP:
        return f"gte_op(c, 0x{ins.cop2:08X}u);"
    if k == D.GTE_LOAD:   # lwc2: cop2_data[rt] = mem[rs+imm]
        return f"gte_write_data({ins.rt}, c->mem_r32({addr_expr(ins)}));"
    if k == D.GTE_STORE:  # swc2: mem[rs+imm] = cop2_data[rt]
        return f"c->mem_w32({addr_expr(ins)}, gte_read_data({ins.rt}));"
    if k == D.COP0:
        if o == "mfc0":
            return wr(ins.rt, f"cop0_mfc(c, {ins.rd})")
        if o == "mtc0":
            return f"cop0_mtc(c, {ins.rd}, {R(ins.rt)});"
        if o == "rfe":
            return "/* rfe (no-op under HLE) */"
    if k == D.SYSCALL:
        return f"rec_syscall(c, {(ins.raw >> 6) & 0xFFFFF}u);"
    if k == D.BREAK:
        return f"rec_break(c, {(ins.raw >> 6) & 0xFFFFF}u);"
    return f"/* UNHANDLED {o} raw=0x{ins.raw:08X} */"


BRANCH_COND = {
    "beq": lambda i: f"{R(i.rs)} == {R(i.rt)}",
    "bne": lambda i: f"{R(i.rs)} != {R(i.rt)}",
    "blez": lambda i: f"(int32_t){R(i.rs)} <= 0",
    "bgtz": lambda i: f"(int32_t){R(i.rs)} > 0",
    "bltz": lambda i: f"(int32_t){R(i.rs)} < 0",
    "bgez": lambda i: f"(int32_t){R(i.rs)} >= 0",
    "bltzal": lambda i: f"(int32_t){R(i.rs)} < 0",
    "bgezal": lambda i: f"(int32_t){R(i.rs)} >= 0",
}


class Names:
    """Symbol naming for one emitted module. Each EXE/overlay that overlaps MAIN.EXE's address space
    gets its OWN module — distinct wrapper/body/dispatch/override symbols + output files — so
    func_<addr> never collides between images that share guest addresses (the boot stub, and the
    OVERLAPPING stage overlays which all load to base 0x80106228). `dispatch` is the module's OWN
    address->fn switch (a unique name per module); `router` is the GLOBAL cross-module dispatch a
    recompiled body CALLS for any target outside its own function set (the hand-written rec_dispatch
    in overlay_router.cpp, which range-routes to the right module's switch). All modules share the
    single router so a call from overlay code into MAIN (or vice versa) resolves correctly."""
    def __init__(self, gen, wrap, dispatch, index, setov, ovtab, decls, shardpfx, disp,
                 router="rec_dispatch"):
        self.gen, self.wrap, self.dispatch = gen, wrap, dispatch
        self.index, self.setov, self.ovtab = index, setov, ovtab
        self.decls, self.shardpfx, self.disp = decls, shardpfx, disp
        self.router = router

MAIN_NAMES = Names("gen_func", "func", "main_dispatch", "rec_func_index", "shard_set_override",
                   "g_override", "rec_decls.h", "shard", "shard_disp")
STUB_NAMES = Names("stub_gen_func", "stub_func", "stub_dispatch", "stub_func_index",
                   "stub_set_override", "g_stub_override", "stub_decls.h", "stub_shard", "stub_disp")

def overlay_names(tag):
    """Per-overlay module names keyed by a short tag (e.g. 'demo' from DEMO.BIN). All overlays share
    the global router (rec_dispatch); each has its own switch ov_<tag>_dispatch."""
    return Names(f"ov_{tag}_gen", f"ov_{tag}_func", f"ov_{tag}_dispatch", f"ov_{tag}_func_index",
                 f"ov_{tag}_set_override", f"g_ov_{tag}_override", f"ov_{tag}_decls.h",
                 f"ov_{tag}_shard", f"ov_{tag}_disp")


def _scan_jt_idiom(ins, a, jr_reg, lo, enhanced):
    """Backward-scan the MIPS switch idiom around the `jr` at `a` (jr reg = jr_reg). Returns
    (base_reg, off, hi_val, lo_add, count) or None. `enhanced` enables two extra forms needed by a few
    functions but which can MISLEAD other recoveries (so the caller tries enhanced ONLY as a fallback):
      - the table base built in a SEPARATE temp reg: `lui tmp,HI ; addiu base,tmp,LO` (rs != rt), and
      - dropping the SCALED-INDEX reg (the `sll` result) from the base-candidate set, so a NEARER
        `lui` that reuses the index reg as scratch isn't mismatched as the table HI.
    See find_jump_tables + docs/findings/sbs.md later-272 (FUN_8003c048's entity-handler table)."""
    base_reg = off = hi_val = lo_add = count = None
    base_regs = set()
    for b in range(a - 4, max(lo, a - 0x40) - 4, -4):
        if b not in ins:
            break
        j = ins[b]
        if base_reg is None and j.op == "lw" and j.rt == jr_reg:    # the table load into the jr reg
            base_reg, off = j.rs, j.simm
            base_regs = {base_reg}
            continue
        if base_reg is not None:
            # `addu B, X, Y` defining a candidate base reg -> either addend can hold the table base
            # (the overlays build the table addr in a separate reg and `addu` it to the index reg).
            if j.op in ("addu", "add") and j.rd in base_regs:
                base_regs |= {j.rs, j.rt}
            # ENHANCED: the scaled index (`sll t, idx, 2` feeding the addu) is never the table base; drop
            # it so a nearer `lui` reusing that reg as scratch isn't matched as the table HI.
            if enhanced and j.op in ("sll", "sllv") and j.rd in base_regs:
                base_regs.discard(j.rd)
            if hi_val is None and j.op == "lui" and j.rt in base_regs:
                hi_val = j.imm << 16
            elif lo_add is None and j.op == "addiu" and j.rt in base_regs and (enhanced or j.rs == j.rt):
                # `addiu base, src, LO`. Classic: src==base. ENHANCED: src may be a separate temp holding
                # the lui'd HI (`lui tmp,HI ; addiu base,tmp,LO`) -> add src so its `lui` is still matched.
                lo_add = j.simm
                if enhanced:
                    base_regs |= {j.rs}
            elif not enhanced and j.op == "addiu" and j.rt in base_regs and j.rs != j.rt:
                # STRICT cannot model `lui tmp,HI ; addiu base,tmp,LO` — the base is built THROUGH a
                # different register. Bail out instead of scanning past it, or the next `lui` for one of
                # the other base candidates is mistaken for the table's HI and the base is read WRONG.
                # MAIN 0x8003BBF8 is exactly that: `lui v0,0x8001 ; addiu s3,v0,0x4A70 ; addu v0,v0,s3 ;
                # lw v0,0(v0) ; jr v0` — strict used to answer 0x80010000 (the LUI alone) and "recover"
                # 144 targets straight out of .text's head, swallowing BOTH the real 22-entry stub table
                # at 0x80010000 AND the head of FUN_80028E10's 22-case switch table at 0x8001021C. The
                # 9 overlapping case labels were then seeded as functions by the cross-boundary seeder,
                # truncating FUN_80028E10 at 0x80028E64 so its own switch emitted `default: rec_dispatch`
                # — and the 12 unseeded cases ABORTED at runtime (recomp-MISS 0x80028FA4/FC4/29004/29024,
                # the object-template init dispatcher; hit on entry to areas 10/11/13/14). Bailing lets
                # the ENHANCED pass resolve the real base (s3 = 0x80014A70).
                return None
        if count is None and j.op == "sltiu":
            count = j.imm
        if hi_val is not None and count is not None:
            break
    if base_reg is None or hi_val is None or off is None or not count or count > 4096:
        return None
    return base_reg, off, hi_val, lo_add, count


def find_jump_tables(exe, ins, lo, hi, validate=True, tbl_spans=None):
    """Recover in-function jump tables (C `switch`) so a computed `jr` stays INSIDE the compiled body
    instead of routing through rec_dispatch (which, under the no-interpreter substrate, would dispatch
    the table's mid-function case labels as fake functions -> stack corruption; see docs/native-port-
    plan.md). Detects the MIPS switch idiom around a `jr rN` (rN != ra), in two compiler variants —
    in both, the lw's base reg B is set by `addu B, X, Y`, and the table address (lui[/addiu]) targets
    B itself OR one of the addends X/Y (the other addend being idx<<2):
        sltiu cond, idx, COUNT ; (beqz cond, default) ; sll t, idx, 2
      (A) lui base,HI ; [addiu base,base,LO] ; addu base,base,t ; lw rN,OFF(base) ; jr rN  (MAIN)
      (B) lui tbl,HI  ; addiu tbl,tbl,LO     ; addu B,t,tbl     ; lw rN,OFF(B)    ; jr rN  (overlays)
    The jump table is at HI<<16 (+LO) + OFF; read COUNT word targets from the EXE image. Returns
    {jr_addr: [target_addr,...]} (targets are the case-label code addresses).

    Per jr we try the STRICT idiom first; only if it yields no readable/in-range table do we retry with
    the ENHANCED idiom (separate-temp base reg + scaled-index exclusion). Strict-first is essential: the
    enhanced heuristics, applied unconditionally, MISLED already-correct recoveries (A00 FUN_80124328's
    switch lost cases -> recomp-MISS 0x80124448). Strict-first means a switch the original logic already
    recovered is byte-identical; enhanced only RESCUES jrs the strict logic missed (FUN_8003c048)."""
    def resolve(scan):
        if scan is None:
            return None
        base_reg, off, hi_val, lo_add, count = scan
        tbl = (hi_val + (lo_add or 0) + off) & 0xFFFFFFFF
        try:
            targets = [exe.word(tbl + k * 4) for k in range(count)]
        except Exception:
            return None
        if validate and any(not (lo <= t < hi) for t in targets):   # a real switch jumps within its fn
            return None
        return tbl, count, targets
    jt = {}
    for a in sorted(ins):
        i = ins[a]
        if not (i.kind == D.JUMPR and i.op == "jr" and i.rs and i.rs != 31):
            continue
        res = resolve(_scan_jt_idiom(ins, a, i.rs, lo, enhanced=False))
        if res is None:
            res = resolve(_scan_jt_idiom(ins, a, i.rs, lo, enhanced=True))
        if res is None:
            continue
        tbl, count, targets = res
        if tbl_spans is not None:                  # the DATA table occupies [tbl, tbl+4*count)
            tbl_spans.add((tbl, count))
        jt[a] = targets
    return jt


def collect_jt_targets(exe, funcs, text_end):
    """Global pre-pass: every jump-table case-label address across all functions. These are mid-function
    code, NOT functions — they must be PRUNED from the function set so the containing function spans its
    whole switch (otherwise the body is truncated at the label, the switch targets fall out of range, and
    recovery fails -> the substrate derails). Validates targets are in-text and decode as real code (so a
    false-positive idiom match on data can't prune a real function)."""
    out = set()
    ordered = sorted(funcs)
    for k, a in enumerate(ordered):
        hi = ordered[k + 1] if k + 1 < len(ordered) else text_end
        ins = {x: decode(x, exe.word(x)) for x in range(a, hi, 4)}
        for jr_a, tgts in find_jump_tables(exe, ins, a, hi, validate=False).items():
            if tgts and all(exe.load <= t < text_end and decode(t, exe.word(t)).kind != D.UNKNOWN
                            for t in tgts):
                out.update(tgts)
    return out


def walk_standalone(ins, lo, hi):
    """The 'standalone' addresses in a CONTIGUOUS run [lo,hi): each is emitted as its own statement; a
    control op consumes the next word as its delay slot, so that word is NOT standalone."""
    st, a = set(), lo
    while a < hi:
        st.add(a)
        a += 8 if ins[a].kind in (D.BRANCH, D.JUMP, D.JUMPR) else 4
    return st


def collect_tail_dups(exe, lo, hi, funcset, ins, jt):
    """Find SHARED-EPILOGUE / tail-merged blocks this function branches to that live OUTSIDE [lo,hi) (a
    sibling tail-merged a common epilogue; A00 0x80113100 -> 0x80113328). A cross-fn branch to such a
    target would route to the dispatcher and fail fast, so we DUPLICATE the block here. We follow ONLY
    out-of-[lo,hi), in-module, NON-entry targets (a sibling ENTRY is a real tail call, left to
    emit_control); we never re-enter [lo,hi) (that's a goto to an existing label). Returns
    (dup_ins, dup_blocks) where dup_blocks = [(start, end, fall_into_main_addr_or_None)] — each a maximal
    contiguous run; fall target set when the run flows back into [lo,hi) (needs a goto)."""
    LO_M, HI_M = exe.load, exe.text_end
    def in_main(x):
        return lo <= x < hi
    def want(t):
        return t is not None and LO_M <= t < HI_M and not in_main(t) and t not in funcset
    dup = {}
    seeds = []
    def gather(src_ins, src_jt):
        for a, i in src_ins.items():
            if i.kind in (D.BRANCH, D.JUMP) and i.op != "jal" and want(i.target):
                seeds.append(i.target)
            if i.kind == D.JUMPR and i.op == "jr":
                seeds.extend(t for t in src_jt.get(a, ()) if want(t))
    gather(ins, jt)
    while seeds:
        T = seeds.pop()
        if T in dup:
            continue
        a = T
        while LO_M <= a < HI_M and not in_main(a) and a not in dup:
            i = decode(a, exe.word(a))
            dup[a] = i
            if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                ds = a + 4
                if LO_M <= ds < HI_M and not in_main(ds) and ds not in dup:
                    dup[ds] = decode(ds, exe.word(ds))
                if i.kind == D.BRANCH:
                    if want(i.target):
                        seeds.append(i.target)
                    a = ds + 4
                elif i.kind == D.JUMP:
                    if i.op == "jal":
                        a = ds + 4
                    else:
                        if want(i.target):
                            seeds.append(i.target)
                        break
                else:
                    break    # jr: terminator (its table, if any, is recovered in the recomputed jt below)
            else:
                a += 4
    # recompute jt over main+dup so a jr inside a dup block gets its switch; re-gather any new tail targets
    if dup:
        alljt = find_jump_tables(exe, {**ins, **dup}, lo, HI_M, validate=True)
        more = []
        for a, i in dup.items():
            if i.kind == D.JUMPR and i.op == "jr":
                more.extend(t for t in alljt.get(a, ()) if want(t) and t not in dup)
        seeds = more
        while seeds:
            T = seeds.pop()
            if T in dup:
                continue
            a = T
            while LO_M <= a < HI_M and not in_main(a) and a not in dup:
                i = decode(a, exe.word(a)); dup[a] = i
                if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                    ds = a + 4
                    if LO_M <= ds < HI_M and not in_main(ds) and ds not in dup:
                        dup[ds] = decode(ds, exe.word(ds))
                    if i.kind == D.BRANCH:
                        a = ds + 4
                    elif i.kind == D.JUMP and i.op == "jal":
                        a = ds + 4
                    else:
                        break
                else:
                    a += 4
    # form maximal contiguous runs; record where a run falls through into [lo,hi)
    blocks = []
    addrs = sorted(dup)
    k = 0
    while k < len(addrs):
        s = addrs[k]
        e = s
        while e in dup:
            e += 4
        fall = e if in_main(e) else None      # the run flows back into the main body -> goto needed
        blocks.append((s, e, fall))
        while k < len(addrs) and addrs[k] < e:
            k += 1
    return dup, blocks


def emit_func(exe, lo, hi, funcset, out, name, N, reentry=()):
    """Emit one C function: the proven LINEAR walk over the contiguous body [lo,hi), PLUS appended
    DUPLICATED tail blocks for shared-epilogue targets that live outside [lo,hi) (collect_tail_dups).
    The [lo,hi) emission is unchanged; the entry (lo) is always first; tails are reached only by goto.
    (A whole-function CFG flood-fill was tried and reverted — it mis-recompiled a register-jump-table
    state machine into an infinite loop; this is the additive, minimal version.)"""
    ins = {a: decode(a, exe.word(a)) for a in range(lo, hi, 4)}
    jt = find_jump_tables(exe, ins, lo, hi)
    dup_ins, dup_blocks = collect_tail_dups(exe, lo, hi, funcset, ins, jt)
    if dup_ins:
        ins = {**ins, **dup_ins}
        jt = find_jump_tables(exe, ins, lo, exe.text_end, validate=True)

    standalone = walk_standalone(ins, lo, hi)
    for s, e, _ in dup_blocks:
        standalone |= walk_standalone(ins, s, e)
    # targets that may be labels: branch/jump targets + jump-table case labels, anywhere in the covered
    # set (main body + duplicated tails). A duplicated tail is reachable, so its targets are real labels.
    all_targets = {i.target for x, i in ins.items()
                   if i.kind in (D.BRANCH, D.JUMP) and i.target is not None and i.target in ins}
    for x, tgts in jt.items():
        if x in ins:
            all_targets |= {t for t in tgts if t in ins}
    labels = {t for t in all_targets if t in standalone}
    # BRANCH-INTO-DELAY-SLOT (MAIN 0x80084080: a `bltz` jumps to 0x800840C8, the delay slot of the
    # preceding unconditional `beq $0,$0`). Such a target is NOT standalone, so without help it routes to
    # the dispatcher. Emit a label for it: after the owning control op drop `L_<ds>: <slot>` (guarded by a
    # skip-goto so the op's own fall-through doesn't re-run the slot); a jump INTO the delay slot runs the
    # slot then falls through. (The slot instruction is duplicated — harmless, a single non-control op.)
    ctrl_delayslots = {a2 + 4 for a2 in standalone
                       if ins[a2].kind in (D.BRANCH, D.JUMP, D.JUMPR)
                       and (a2 + 4) in ins and ins[a2 + 4].kind not in (D.BRANCH, D.JUMP, D.JUMPR)}
    ds_label_targets = {t for t in all_targets if t not in standalone and t in ctrl_delayslots}
    labels |= ds_label_targets
    # a duplicated tail that flows back into the main body jumps to L_<fall>; force that to be a label.
    labels |= {fall for _, _, fall in dup_blocks if fall is not None}

    def emit_run(s, e):
        a = s
        while a < e:
            i = ins[a]
            if a in labels:
                out.append(f"L_{a:08X}:;")
            if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                slot = ins.get(a + 4)
                ds_c = emit_simple(slot) if (slot and slot.kind not in
                       (D.BRANCH, D.JUMP, D.JUMPR)) else "/* DS */"
                out.extend(emit_control(i, ds_c, funcset, labels, N, jt.get(a)))
                if (a + 4) in ds_label_targets:        # the delay slot is also a branch target
                    out.append(f"  goto L_DSAFTER_{a:08X};")
                    out.append(f"L_{a + 4:08X}:; {ds_c}")
                    out.append(f"L_DSAFTER_{a:08X}:;")
                a += 8
            else:
                st = emit_simple(i)
                if st:
                    out.append("  " + st)
                a += 4

    # A function cut at a DELIBERATE mid-function RE-ENTRY SEED (reentry) whose body runs off its end into
    # that seed must CONTINUE into it, not `return`. Case: GAME's prologue 0x8010637C falls through into its
    # cooperative-loop top 0x801063F4 (both seeded so the native game_coop path can re-enter the loop top per
    # frame). With a bare `return`, the FULL-PSX coro runs the prologue, returns, and the scheduler reaps the
    # task as "done" — the title-Start field freeze. Emit a TAIL-CALL to `hi` on genuine fall-through (the
    # nested call preserves the C stack, so a deep yield inside hi suspends correctly on the coro). SCOPED to
    # `hi in reentry`: a natural function boundary (most fns end in `jr ra`) is unaffected, and we don't
    # execute previously-unreached fall-through code in other overlays (which would surface jal targets the
    # discovery scan never seeded -> recomp-MISS). `hi` falls through unless the body's last instruction is
    # an unconditional transfer (`j`/`jr`); a normal insn / conditional branch / `jal` all reach `hi`.
    def body_falls_through():
        a, last = lo, None
        while a < hi:
            i = ins.get(a)
            if i is None:
                return False                # a gap/data hole — be conservative, don't tail-call
            last = i
            a += 8 if i.kind in (D.BRANCH, D.JUMP, D.JUMPR) else 4
        if last is None:
            return False
        if last.kind == D.JUMPR:            # jr (jr ra or computed) — unconditional, no fall-through
            return False
        if last.kind == D.JUMP and last.op == "j":   # unconditional j — no fall-through
            return False
        return True                          # normal / conditional branch / jal -> reaches hi
    out.append(f"void {name}(Core* c) {{")
    emit_run(lo, hi)                        # the proven contiguous body — entry (lo) always first
    if hi in funcset and body_falls_through():
        # GENUINE FALL-THROUGH into the next function (hi). Chain to it (a nested call preserves the C
        # stack so a deep yield inside hi suspends correctly on the coro). This models real MIPS execution
        # for a function SPLIT mid-body — e.g. a jump-table case fragment (0x80022854) whose last insn is a
        # `jal` and which then falls through into the next case/epilogue fragment (0x8002285c). A bare
        # `return` here DROPS that control-flow edge: the shared-frame epilogue (addiu sp,+N; jr ra) in the
        # fall-through fragment never runs, LEAKING the caller's stack frame every time the path is taken
        # (later-286: 0x28/frame guest-sp leak overflowed task0's stack into the task table after ~50 field
        # frames -> free-roam recomp-MISS crash). SCOPED by body_falls_through(): a normal `jr ra`/`j`
        # terminator returns False, so a natural function boundary is unaffected; only a body whose last
        # instruction actually reaches `hi` (normal insn / conditional branch / jal) chains. If chaining
        # surfaces an unseeded jal target -> recomp-MISS, that reveals a REAL reachable path to seed, which
        # is correct (fail-fast) — far better than silently corrupting the guest stack.
        out.append(f"  {call_or_dispatch(hi, funcset, N)} return;")  # fall-through into the next fragment
    else:
        out.append("  return;")
    # DUPLICATED shared-epilogue tails (collect_tail_dups), reached only via goto from the body / a tail.
    # Each ends at its own terminator; a run that falls back into the main body gets an explicit goto.
    for s, e, fall in dup_blocks:
        emit_run(s, e)
        if fall is not None:
            out.append(f"  goto L_{fall:08X};")
        else:
            out.append("  return;")
    out.append("}")
    out.append("")


def call_or_dispatch(target, funcset, N):
    return (f"{N.wrap}_{target:08X}(c);" if target in funcset
            else f"{N.router}(c, 0x{target:08X}u);")


def emit_control(i, ds_c, funcset, labels, N, jtargets=None):
    """Lines for a control instruction `i` whose delay-slot C is `ds_c`. `jtargets` (if set) = the
    recovered jump-table case-label addresses for a computed `jr` -> emit a C switch on the target
    value (auto-dedupes repeated entries) so the jump stays inside this compiled body."""
    L = []
    if i.kind == D.BRANCH:
        if i.op in ("bltzal", "bgezal"):
            L.append(f"  {R(31)} = 0x{i.addr + 8:08X}u;")
        cond = BRANCH_COND[i.op](i)
        if i.target in labels:
            tgt = f"goto L_{i.target:08X};"
        else:
            tgt = "{ " + call_or_dispatch(i.target, funcset, N) + " return; }"
        L.append(f"  {{ int _t = ({cond}); {ds_c} if (_t) {tgt} }}")
        return L
    if i.kind == D.JUMP:
        if i.op == "jal":
            L.append(f"  {R(31)} = 0x{i.addr + 8:08X}u;")
            L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset, N)}")
        else:  # j
            if i.target in labels:
                L.append(f"  {ds_c} goto L_{i.target:08X};")
            else:
                L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset, N)} return;")
        return L
    # JUMPR
    if i.op == "jr":
        if i.rs == 31:
            L.append(f"  {ds_c} return;")
        elif jtargets:
            # recovered jump table: switch on the loaded target value -> goto the case label. Dedupe
            # repeated targets (a C switch can't have two identical case values). default = dispatch
            # (unreached: the preceding bounds-check `beqz` guards the index range).
            seen, cases = set(), []
            for t in jtargets:
                if t in seen or t not in labels:
                    continue
                seen.add(t)
                cases.append(f"case 0x{t:08X}u: goto L_{t:08X};")
            L.append(f"  {{ {ds_c} switch ({R(i.rs)}) {{ {' '.join(cases)} "
                     f"default: {N.router}(c, {R(i.rs)}); return; }} }}")
        else:
            L.append(f"  {ds_c} {N.router}(c, {R(i.rs)}); return;")
    else:  # jalr rd, rs
        L.append(f"  {R(i.rd)} = 0x{i.addr + 8:08X}u;")
        L.append(f"  {ds_c} {N.router}(c, {R(i.rs)});")
    return L


def ghidra_funcs(text_lo, text_hi, decomp="scratch/decomp/ram_f1000_all.c"):
    """OPTIONAL extra seeds from a LOCAL Ghidra decomp (a gitignored scratch artifact), used only
    under PSXPORT_USE_GHIDRA to recompile more functions for speed. The default build does NOT use
    this — it is binary-only (entry + jal discovery) so no decomp-derived data is needed or shipped.
    Returns [] when the decomp isn't present."""
    if not os.path.exists(decomp):
        return []
    return sorted({int(x, 16) for x in re.findall(
        r'==================== ([0-9A-F]{8}) FUN_', open(decomp).read())
        if text_lo <= int(x, 16) < text_hi})


def overlay_funcs(exe, overlay_dir, base=0x80106228):
    """Seed resident functions that are reached ONLY from the stage overlays (\\BIN\\*.BIN, loaded
    raw to `base` at runtime and run by the interpreter). Those overlays `jal` into resident
    MAIN.EXE functions (the cooperative scheduler, file loaders, etc.) that direct-jal discovery
    within MAIN.EXE never sees — e.g. FUN_80044bd4. We scan each overlay's words for `jal` targets
    landing in MAIN.EXE text whose first word decodes as a real instruction (filters data/jump-
    table false positives), and seed them; discover_funcs then follows their call graph. Pure
    binary input (the overlays are game data, like MAIN.EXE) — no Ghidra. Skipped if absent."""
    lo, hi = exe.load, exe.text_end
    if not overlay_dir or not os.path.isdir(overlay_dir):
        return set()
    targets = set()
    for fn in sorted(os.listdir(overlay_dir)):
        if not fn.upper().endswith(".BIN"):
            continue
        data = open(os.path.join(overlay_dir, fn), "rb").read()
        for off in range(0, len(data) - 3, 4):
            w = int.from_bytes(data[off:off + 4], "little")
            ins = decode(base + off, w)
            if ins.op == "jal" and lo <= ins.target < hi:
                if decode(ins.target, exe.word(ins.target)).kind != D.UNKNOWN:
                    targets.add(ins.target)
    return targets


def overlay_data_func_pointers(exe, overlay_dir):
    """Seed resident MAIN functions the \\BIN\\*.BIN overlays reference via a runtime function-POINTER
    that neither MAIN's jal-discovery nor overlay_funcs' jal-scan can see. Two ways an overlay names a
    MAIN handler/task-entry, both covered here:
      (a) as a DATA WORD — per-object TEMPLATES whose behavior-handler field points into MAIN code (the
          FUN_739ac / FUN_73cd8 … object-behavior family); the engine copies the template into an object,
          then a dispatcher (FUN_8007A904) jalrs the pointer.
      (b) BUILT IN OVERLAY CODE — `lui rX,H; addiu/ori rX,…,L` reconstructing a MAIN function address that
          is then registered as a cooperative TASK ENTRY (obj+0xc) and dispatched fresh by the scheduler
          (ra=DEAD0000), e.g. START.BIN→0x8004514C, DEMO/GAME.BIN→0x800452C0.
    Either way the no-interpreter substrate fail-fasts on the un-recompiled target (the attract demo plays
    an area and hits exactly these). Seed every MAIN function ENTRY (is_func_entry) so named; discover_funcs
    follows each one's call graph. Bounded, fully general — no per-address EXTRA_SEEDS whack-a-mole."""
    if not overlay_dir or not os.path.isdir(overlay_dir):
        return set()
    lo, hi = exe.load, exe.text_end
    out = set()
    for fn in sorted(os.listdir(overlay_dir)):
        if not fn.upper().endswith(".BIN"):
            continue
        data = open(os.path.join(overlay_dir, fn), "rb").read()
        words = [int.from_bytes(data[o:o + 4], "little") for o in range(0, len(data) & ~3, 4)]
        for idx, w in enumerate(words):
            # (a) data word that is a MAIN function entry
            if lo <= w < hi and is_func_entry(exe, w):
                out.add(w)
            # (b) lui rX,H followed (short window, same dest reg) by addiu/ori L -> a MAIN function entry
            ins = decode(0, w)   # PC irrelevant for lui/addiu immediate reconstruction
            if ins.op != "lui":
                continue
            rd, H = ins.rt, ins.imm
            for j2 in range(idx + 1, min(idx + 6, len(words))):
                k = decode(0, words[j2])
                if getattr(k, "op", None) in ("addiu", "ori") and getattr(k, "rt", None) == rd \
                   and getattr(k, "rs", None) == rd:
                    val = ((H << 16) + k.simm) & 0xFFFFFFFF if k.op == "addiu" else ((H << 16) | (k.imm & 0xFFFF))
                    if lo <= val < hi and is_func_entry(exe, val):
                        out.add(val)
                    break
    return out


def is_func_entry(exe, w):
    """Does in-text address `w` look like a FUNCTION ENTRY? Two strong, independent signals:
      (a) standard prologue `addiu sp, sp, -N` (0x27BD8000 mask, negative imm), or
      (b) the word at w-8 is `jr ra` (0x03E00008) — i.e. w starts right after the previous function's
          return + delay slot (catches STACKLESS LEAF fns that start with lui/lw, no frame setup).
    Requires w in text, 4-aligned, and decoding as a real instruction (filters data)."""
    lo, hi = exe.load, exe.text_end
    if not (lo <= w < hi and (w & 3) == 0):
        return False
    if decode(w, exe.word(w)).kind == D.UNKNOWN:
        return False
    if (exe.word(w) & 0xFFFF8000) == 0x27BD8000:        # (a) addiu sp, sp, -N
        return True
    if w - 8 >= lo and exe.word(w - 8) == 0x03E00008:   # (b) preceded by `jr ra; <delay>`
        return True
    return False


def func_entries_after_return(exe):
    """Overlay function-boundary scan: every in-text address whose preceding two words are
    `jr ra; <delay>` — i.e. a function starts right after the previous one returns. For a self-
    contained overlay blob (no symbol table, reached via computed `jr`/`j` not just `jal`) this
    recovers the contiguous function layout directly. Unlike a blanket is_func_entry scan it does
    NOT false-positive on a mid-prologue `addiu sp` (only signal (b)), so a lui/lbu-prologue function
    like 0x80106F80 is seeded at its true start, not split. decode filters trailing data."""
    lo, hi = exe.load, exe.text_end
    return {a for a in range(lo + 8, hi, 4)
            if exe.word(a - 8) == 0x03E00008 and decode(a, exe.word(a)).kind != D.UNKNOWN}


def pointer_table_funcs(exe, exclude=None):
    """Seed functions reached ONLY via a function pointer (jalr through a table / vtable slot) —
    invisible to direct-jal discovery. With the interpreter gone (later-254) a call to such a fn fails
    fast, so we must recompile them. Scan the WHOLE EXE image (text + data) for words that point at a
    function ENTRY in text (is_func_entry). discover_funcs then follows each one's direct-jal call
    graph. (A truly stackless leaf that is also NOT preceded by `jr ra` — e.g. the first fn after a
    data island — still needs a manual EXTRA_SEEDS when the boot surfaces it.)

    `exclude` (defaults to switch_table_spans) is SKIPPED so a switch's case-label array is not
    mis-seeded as a vtable. Without this, JT case labels preceded by `jr ra; nop` (each prior case's
    return) pass is_func_entry and get seeded as HARD functions, splitting the switch's containing
    function so early that emit_func's validated JT recovery rejects the switch (targets outside
    range) — the jr then routes through rec_dispatch and case-0 (NOT preceded by `jr ra`, so never
    seeded) fails fast (FUN_8006C80C's 13-entry JT at 0x80016874 → miss 0x8006c844)."""
    lo, hi = exe.load, exe.text_end
    exclude = exclude if exclude is not None else switch_table_spans(exe)
    return {exe.word(a) for a in range(lo, hi, 4)
            if a not in exclude and is_func_entry(exe, exe.word(a))}


def switch_table_spans(exe):
    """All in-function SWITCH jump-table data spans (set of guest addresses occupied by the word
    tables a `jr` indexes). Scanned over the WHOLE text (no function boundaries) via the same idiom
    matcher find_jump_tables uses. code_pointer_tables excludes these so a switch's case-label array
    (a run of in-text code addresses) is NOT mistaken for a vtable and seeded as fake functions —
    which would shred the containing function (e.g. the printf/format parser 0x8009A76C)."""
    lo, hi = exe.load, exe.text_end
    ins = {a: decode(a, exe.word(a)) for a in range(lo, hi, 4)}
    spans = set()
    find_jump_tables(exe, ins, lo, hi, validate=True, tbl_spans=spans)
    occupied = set()
    for tbl, count in spans:
        occupied.update(range(tbl, tbl + 4 * count, 4))
    return occupied


def code_pointer_tables(exe, min_run=4, exclude=None):
    """Seed functions reached via a function-POINTER TABLE (a vtable / handler dispatch table) whose
    entries don't match is_func_entry — e.g. stackless leaf handlers that start with `lw`/`addu`, not
    an `addiu sp` prologue, and aren't preceded by `jr ra` (so neither pointer_table_funcs nor the
    boundary scan sees them). A run of >=min_run CONSECUTIVE 4-aligned image words that all point into
    text and decode as real instructions at their targets is an unambiguous code-pointer table (a lone
    in-text-looking data word is not — hence the run requirement). Seed every target in such a run;
    discover_funcs then follows their call graphs. Catches the DEMO menu / scene-handler vtables that
    the no-interpreter substrate would otherwise fail-fast on (each entry is jalr'd, never jal'd).
    `exclude` (a set of addresses, from switch_table_spans) is SKIPPED so a switch's case-label array
    is not mis-seeded as a vtable (its targets are in-function labels, recovered as `goto`s instead)."""
    lo, hi = exe.load, exe.text_end
    exclude = exclude if exclude is not None else switch_table_spans(exe)
    out, run, a = set(), [], lo
    def intext_code(w):
        return lo <= w < hi and (w & 3) == 0 and decode(w, exe.word(w)).kind != D.UNKNOWN
    while a + 4 <= hi:
        w = exe.word(a)
        if a not in exclude and intext_code(w):
            run.append(w)
        else:
            if len(run) >= min_run:
                out.update(run)
            run = []
        a += 4
    if len(run) >= min_run:
        out.update(run)
    return out


def constructed_func_pointers(exe):
    """Seed functions whose pointer is BUILT IN CODE (`lui rD, H; addiu/ori rD, rD, L`) and stored into
    a vtable / passed as a callback — so the address never appears as a single data word (pointer_table_
    funcs misses it) and it is reached only by jalr (direct-jal discovery misses it). For each `lui`
    followed (within a short window, same dest reg) by an `addiu`/`ori`, reconstruct the 32-bit value
    and seed it if it is a function entry (is_func_entry). addiu sign-extends L; ori zero-extends."""
    lo, hi = exe.load, exe.text_end
    out = set()
    for a in range(lo, hi, 4):
        ins = decode(a, exe.word(a))
        if ins.op != "lui":
            continue
        rd, H = ins.rt, ins.imm
        for b in range(a + 4, min(a + 24, hi - 3), 4):
            j = decode(b, exe.word(b))
            if getattr(j, "op", None) in ("addiu", "ori") and getattr(j, "rt", None) == rd \
               and getattr(j, "rs", None) == rd:
                val = ((H << 16) + j.simm) & 0xFFFFFFFF if j.op == "addiu" else ((H << 16) | (j.imm & 0xFFFF))
                if is_func_entry(exe, val):
                    out.add(val)
                break
    return out


def discover_funcs(exe, seeds):
    """Grow the function set by following direct `jal` targets to a fixpoint. Each function
    body is scanned only up to its first UNKNOWN word (real code is 0% unknown), so trailing
    jump-table/constant data doesn't inject spurious seeds. Catches functions Ghidra missed
    that are reached by a direct call (the recompiler's 'seed indirect-only targets' rule)."""
    lo, hi = exe.load, exe.text_end
    funcs = set(seeds)
    changed = True
    while changed:
        changed = False
        ordered = sorted(funcs)
        for idx, a in enumerate(ordered):
            end = ordered[idx + 1] if idx + 1 < len(ordered) else hi
            for x in range(a, end, 4):
                ins = decode(x, exe.word(x))
                if ins.kind == D.UNKNOWN:
                    break  # start of trailing data
                if ins.op == "jal" and lo <= ins.target < hi and ins.target not in funcs:
                    funcs.add(ins.target)
                    changed = True
    return sorted(funcs)


def merge_early_return_boundaries(exe, funcs, removable, hard):
    """Fix FALSE function boundaries from func_entries_after_return: a `jr ra` is NOT always a function
    end — a function may have an EARLY RETURN mid-body (an `if(...) return;`) or several returns sharing a
    tail epilogue, and continue/branch across that point. The boundary scan would split it there, so an
    intra-function branch/`j` that targets PAST the split routes through the router -> fail-fast (A00
    0x80131600 branches to its 0x801316C4 tail; 0x801130C4 branches to the 0x80113328 shared epilogue).
    A soft boundary g is FALSE if ANY instruction in the REAL function it sits inside — [H, g), where H is
    the nearest HARD entry (real jal/pointer/explicit function start) at or before g — has a forward
    branch/`j` whose target lands in [g, next). Drop such g (merging extends the function); only
    `removable` boundaries (soft seeds, never a hard entry) are dropped. Iterate to a fixpoint."""
    funcs = sorted(funcs)
    hard = set(hard)
    changed = True
    while changed:
        changed = False
        for idx in range(1, len(funcs)):
            g = funcs[idx]
            if g not in removable:
                continue
            nxt = funcs[idx + 1] if idx + 1 < len(funcs) else exe.text_end
            # scan back to the nearest HARD entry (the real function start this g sits inside)
            j = idx - 1
            while j > 0 and funcs[j] not in hard:
                j -= 1
            crosses = False
            for x in range(funcs[j], g, 4):
                i = decode(x, exe.word(x))
                if (i.kind == D.BRANCH or i.op == "j") and i.target is not None and g <= i.target < nxt:
                    crosses = True
                    break
            if crosses:
                funcs.pop(idx)
                changed = True
                break
    return funcs


def emit_module(exe, out_dir, N, seeds, ov_dir=None, limit=None, shards=8, soft_seeds=None, reentry=()):
    """Discover the recompiled function set for `exe` and emit its module (shards + dispatch TU +
    decls header) under the symbol/file names in `N`. Shared by the MAIN.EXE module and the boot-
    stub module; the stub gets distinct names so its func_<addr> don't collide with MAIN's."""
    ov = overlay_funcs(exe, ov_dir)
    if ov:
        print(f"[{N.wrap}] overlay seeds: {len(ov)} resident fns reached from {ov_dir}")
    hard = set(seeds) | ov                       # real call targets (never merged away)
    soft = set(soft_seeds or ())                 # func_entries_after_return boundaries (mergeable)
    if os.environ.get("PSXPORT_USE_GHIDRA"):
        hard |= set(ghidra_funcs(exe.load, exe.text_end))
    seeds = hard                                  # for the jt-prune "keep seeds" guard below
    funcs = discover_funcs(exe, hard | soft)
    # PRUNE jump-table case labels wrongly seeded/discovered as functions: they are mid-function code,
    # and leaving them in truncates the containing function so its switch can't be recovered (the
    # substrate-derail root cause). Keep any that ARE a seed entry (defensive: a real fn shouldn't be a
    # case label, but never drop an explicit seed).
    jt_labels = collect_jt_targets(exe, funcs, exe.text_end) - set(seeds)
    if jt_labels:
        funcs = [f for f in funcs if f not in jt_labels]
        print(f"[{N.wrap}] pruned {len(jt_labels)} jump-table case labels from the function set")
    # Merge FALSE early-return boundaries (soft func_entries_after_return seeds that split a function with
    # a mid-body `jr ra`). removable = soft seeds that are NOT a real call target (jal/pointer entry).
    if soft_seeds:
        lo, hi = exe.load, exe.text_end
        jal_tgts = {decode(a, exe.word(a)).target for a in range(lo, hi, 4)
                    if decode(a, exe.word(a)).op == "jal"}
        # A soft boundary that allocates its OWN stack frame (`addiu sp, sp, -N` prologue) is a GENUINE
        # function entry, not a mid-body early-return continuation (which keeps using the existing frame).
        # NEVER merge it: an overlay handler reached ONLY via a runtime object-method pointer is invisible
        # to static jal discovery, so merging it away makes that computed call fail-fast (recomp-MISS
        # 0x80146478 — the narration-cutscene actor's update method, an `addiu sp,sp,-0x20` fn). The merge's
        # real targets (shared epilogues / mid-body early returns: 0x80113328, 0x801316C4) have NO prologue.
        has_prologue = {f for f in soft_seeds if (exe.word(f) & 0xFFFF8000) == 0x27BD8000}
        removable = (set(soft_seeds) - set(seeds) - has_prologue) - jal_tgts
        before = len(funcs)
        funcs = merge_early_return_boundaries(exe, funcs, removable, set(seeds) | jal_tgts)
        if len(funcs) != before:
            print(f"[{N.wrap}] merged {before - len(funcs)} false early-return boundaries")
    # (Shared-epilogue / cross-function branch targets are NOT seeded as separate functions — emit_func's
    # CFG flood-fill DUPLICATES such tails into each function that branches to them, so a function is never
    # split at a point its own internal branches cross. floodfill_func is the single source of truth for a
    # function's instruction extent.)
    # CROSS-BOUNDARY SWITCH TARGETS (later-272): a jump-table case that lands OUTSIDE the jr's own function
    # — because a SIBLING function entry (a real pointer-table fn) splits the switch — is unreachable as an
    # in-function `goto`; emit_func routes it via `default: rec_dispatch(target)`. So that target MUST be a
    # function ENTRY or the dispatch fails fast (recomp-MISS) the moment the game's runtime index selects it
    # (A00 jr 0x80124354 -> 0x80124448/0x80124488, which fall inside sibling fn 0x801243E8). Seed every such
    # cross-boundary target so rec_dispatch resolves it (faithful: it runs from that label exactly as the
    # original `jr` did). This was always latent; the FUN_8003c048 jump-table fix merely lets the field
    # progress far enough to reach these cases.
    import bisect as _bisect
    fs = sorted(funcs)
    fs_set = set(fs)
    def _fn_hi(addr):
        k = _bisect.bisect_right(fs, addr)
        return fs[k] if k < len(fs) else exe.text_end
    xb = set()
    for a in fs:
        hi_f = _fn_hi(a)
        ins_f = {x: decode(x, exe.word(x)) for x in range(a, hi_f, 4)}
        for jr, tgts in find_jump_tables(exe, ins_f, a, hi_f, validate=False).items():
            for t in tgts:
                if not (a <= t < hi_f) and t not in fs_set and exe.load <= t < exe.text_end \
                        and decode(t, exe.word(t)).kind != D.UNKNOWN:
                    xb.add(t)
    if xb:
        funcs = sorted(set(funcs) | xb)
        print(f"[{N.wrap}] seeded {len(xb)} cross-boundary switch targets (else recomp-MISS via switch default)")
    print(f"[{N.wrap}] functions: {len(seeds)} seeds -> {len(funcs)} recompiled after jal "
          f"discovery (a call to any non-recompiled address fails fast at runtime)")
    funcset = set(funcs)
    if limit:
        funcs = funcs[:limit]

    ordered = sorted(funcset)
    nxt_of = {f: (ordered[k + 1] if k + 1 < len(ordered) else exe.text_end)
              for k, f in enumerate(ordered)}
    idx = {a: k for k, a in enumerate(funcs)}

    hdr = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", "#pragma once",
           '#include "core.h"', ""]
    for a in funcs:
        hdr.append(f"void {N.gen}_{a:08X}(Core*); void {N.wrap}_{a:08X}(Core*);")
    hdr.append(f"extern OverrideFn {N.ovtab}[];")
    hdr.append(f"void {N.dispatch}(Core* c, uint32_t addr);")
    hdr.append(f"void {N.setov}(uint32_t addr, OverrideFn fn);")
    open(os.path.join(out_dir, N.decls), "w").write("\n".join(hdr) + "\n")

    shard = [["// GENERATED — DO NOT EDIT.", f'#include "{N.decls}"', ""] for _ in range(shards)]
    for k, a in enumerate(funcs):
        emit_func(exe, a, nxt_of[a], funcset, shard[k % shards], f"{N.gen}_{a:08X}", N, reentry)
    for s in range(shards):
        open(os.path.join(out_dir, f"{N.shardpfx}_{s}.c"), "w").write("\n".join(shard[s]) + "\n")

    # Dispatch TU: override table + wrappers + the address->fn switches. Overrides super-call the
    # gen body directly to run the original without re-entering the wrapper. Dispatch misses fall
    # to the shared rec_dispatch_miss (BIOS vectors / overlay / computed targets via interp).
    d = ["// GENERATED — DO NOT EDIT.", f'#include "{N.decls}"', "",
         f"OverrideFn {N.ovtab}[{max(1, len(funcs))}];"]
    for a in funcs:
        i = idx[a]
        # c->pc = this function's guest address — the PER-CORE program counter (r3000.h). Set on
        # every recompiled-function entry so mem-watch / store-trap / backtrace diagnostics report
        # which guest function THIS core is in (the substrate's analog of the old interpreter PC).
        d.append(f"void {N.wrap}_{a:08X}(Core* c) {{ c->pc = 0x{a:08X}u; if ({N.ovtab}[{i}]) {{ "
                 f"{N.ovtab}[{i}](c); return; }} {N.gen}_{a:08X}(c); }}")
    d.append(f"int {N.index}(uint32_t addr) {{\n  switch (addr & 0x1FFFFFFFu) {{")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: return {idx[a]};")
    d.append("    default: return -1;\n  }\n}")
    d.append(f"void {N.setov}(uint32_t addr, OverrideFn fn) "
             f"{{ int i = {N.index}(addr); if (i >= 0) {N.ovtab}[i] = fn; }}")
    d.append(f"void {N.dispatch}(Core* c, uint32_t addr) {{\n  switch (addr & 0x1FFFFFFFu) {{")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: {N.wrap}_{a:08X}(c); return;")
    d.append("    default: rec_dispatch_miss(c, addr); return;\n  }\n}")
    open(os.path.join(out_dir, f"{N.disp}.c"), "w").write("\n".join(d) + "\n")
    print(f"[{N.wrap}] emitted {len(funcs)} functions -> {out_dir}/{N.shardpfx}_*.c "
          f"({shards} shards) + {N.decls}")
    # The source TUs this module wrote (basenames) — collected into generated/rec_sources.cmake so
    # the build links exactly what was emitted (overlay count is dynamic).
    return [f"{N.disp}.c"] + [f"{N.shardpfx}_{s}.c" for s in range(shards)]


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: emit.py <MAIN.EXE> <out.c> [--overlays DIR] [--stub SCUS.EXE] [--limit N]")
    exe = psexe.load(sys.argv[1])
    out_path = sys.argv[2]
    limit = None
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])

    # Functions reached only indirectly (jalr through a function pointer) — invisible to
    # direct-jal discovery, found empirically via boot dispatch-misses. Documented seeds.
    # NOTE: 0x8009A8E8/ADC4/AA4C were NOT functions — they are mid-function jump-table
    # (switch) labels inside the printf/format-parser at 0x8009A76C, surfaced as misses
    # because computed `jr` is routed to rec_dispatch (no in-function jump-table recovery
    # yet). The real entry is seeded instead; the parser still needs jump-table recovery
    # OR a native printf override to work (see docs/recomp_port_plan.md).
    EXTRA_SEEDS = {
        0x8009A76C,  # printf/format-parser, reached only via fn-pointer (jalr), Ghidra-missed
        # --- FULL-PSX (psx_fallback) cooperative TASK ENTRIES: stored at task-obj+0xc by a runtime
        #     task-register call and reached only as the scheduler's fresh-entry dispatch (ra=DEAD0000),
        #     so neither direct-jal nor the pointer scans find them. Needed for PSXPORT_SBS_MODE=both/
        #     gameplay (full-PSX core B); the native path owns these stages so it never dispatches them.
        #     Surfaced empirically by the full-PSX miss-loop (later-264).
        # (the object-behavior handler family 0x800739AC/0x80073CD8/… AND the overlay-registered task
        #  entries 0x8004514C/0x800452C0/… are seeded GENERALLY by overlay_data_func_pointers — handler
        #  pointers live in area-overlay object templates; task entries are lui+addiu-built in overlay code.)
        # --- engine render functions reached ONLY via a function pointer (so direct-jal discovery
        #     misses them) — seeded to EMIT readable C for the native-engine RE/port (later-135). The
        #     runtime is interpreter-only, so seeding doesn't change execution; it makes the body
        #     available in generated/ for RE (and rec_set_override works on them by address regardless).
        0x8002AB5C,  # field terrain/map renderer — node+24 render-fn ptr of the t32 render-list node
        0x80051C8C,  # per-object transform builder (node+0x98 matrix from euler angles + position)
        # --- per-object RENDER sub-handlers reached ONLY via a runtime handler pointer (the perobj
        #     render dispatcher FUN_8003CCA4 `jr v0` where v0 = a node's render-cmd fn ptr, built at
        #     area-load into guest data — NOT the static table 0x80014EC8). They are ALSO jump-table
        #     case labels of the sibling FUN_8003D584's own switch, so discovery treated them as
        #     mid-function labels and did NOT emit function entries; a runtime dispatch to them then
        #     recomp-MISSes via FUN_8003CCA4's switch default. Latent until the task0 stack-leak fix
        #     (later-286) let free-roam run long enough to render the object that carries this handler.
        # --- Mid-function COROUTINE RESUME target inside the yield primitive FUN_80051F80. When a
        #     task calls FUN_80051F80 to yield, the substrate scheduler captures ra = the instruction
        #     AFTER `jal 0x80080880` inside 51F80 (i.e. 0x80051FA4). Resuming the coroutine dispatches
        #     that ra as a function entry, but 0x80051FA4 is a mid-function label — not naturally in
        #     the fn set. Seed it so a resume-target dispatch has a body to enter. Surfaced by SBS
        #     gameplay-mode f0 fresh-entry rec_coro_run(0x8010649C) → yield → resume-miss.
        0x80051FA4,
        # --- AREA-MODE stub table (0x80010000, 22 entries): each entry is a tiny resident stub that
        #     `jal`s one overlay leaf and falls through to the shared epilogue 0x8001CB98. The guest
        #     reaches them through the `jr` in FUN_8001CAC0, so the jump-table matcher classifies them
        #     as that function's switch case labels and prunes them. Tomba2Engine's native
        #     Engine::areaModeDispatchFaithful dispatches 0x8001CB98 BY ADDRESS, and the whole family is
        #     documented/mirrored there, so they must stay real function entries.
        0x8001CB00, 0x8001CB10, 0x8001CB20, 0x8001CB30, 0x8001CB40, 0x8001CB50,
        0x8001CB60, 0x8001CB70, 0x8001CB80, 0x8001CB90, 0x8001CB98,
        0x8003D5CC,  # FUN_8003CCA4/FUN_8003D584 perobj render sub-handler (switch case target)
        0x8003D8AC,  # FUN_8003CCA4/FUN_8003D584 perobj render sub-handler (switch case target) — the observed miss
        # --- native-override targets: seed so the func_<addr> wrapper exists and
        #     rec_set_override(addr,fn) reaches them (rec_set_override only works on RECOMPILED
        #     entries). libcard B0-vector I/O trampolines, overridden by memcard.c for native
        #     synchronous card file I/O (see runtime/recomp/memcard.c). All three are already
        #     reachable via direct jal (so this is a harmless no-op today), seeded proactively
        #     so the override contract never depends on jal-discovery happening to reach them.
        0x8009BAF0,  # _card_read   (B0:0x4E) — override target (memcard.c ov_card_read)
        0x8009C600,  # _card_write  (B0:0x4F) — override target (memcard.c ov_card_write)
        0x8009C610,  # _card_status (B0:0x5C) — override target (memcard.c ov_card_status)
        # NOTE (later-254): resident fns reached ONLY via a function pointer (jalr) — invisible to
        # direct-jal discovery — are now auto-seeded by pointer_table_funcs + constructed_func_pointers
        # (data-word AND lui+addiu code-built vtable pointers). Add a manual seed here ONLY for a fn
        # those two scans can't see (e.g. a stackless leaf not preceded by `jr ra`, or a pointer built
        # by an unusual instruction sequence) when the fail-fast boot surfaces it as a [recomp-MISS].
        # NOTE: 0x80003A4C (per-VBlank pad read FUN_80003a4c) is intentionally NOT seeded here.
        # It lives at 0x80003A4C, BELOW MAIN.EXE's text [0x80010000,0x800BE800) — it is part of
        # the boot-stub/resident low-text SIO driver loaded at runtime, NOT present in MAIN.EXE
        # (our recompiler input). emit.py can only recompile addresses inside MAIN.EXE's text, so
        # seeding it would raise (decode of an out-of-text vaddr) and break the build. The pad
        # override must therefore be wired differently (it cannot use rec_set_override on a
        # not-in-input address); see report / pad_input.c wiring.
    }
    # Seed purely from the BINARY — the entry point + indirectly-reached helpers — and grow the
    # recompiled set by following direct jal targets (discover_funcs). No Ghidra / external
    # function list. The interpreter is GONE (later-254): a fn reached only via a function pointer
    # (jalr) that discovery can't see is NOT recompiled, so a call to it FAILS FAST at runtime — seed
    # it in EXTRA_SEEDS above when the boot surfaces it. (Set PSXPORT_USE_GHIDRA=1 to additionally
    # seed from the Ghidra decomp / committed list, recompiling more up-front.)
    ov_dir = sys.argv[sys.argv.index("--overlays") + 1] if "--overlays" in sys.argv else None
    seeds = ({exe.entry} | EXTRA_SEEDS | pointer_table_funcs(exe)
             | constructed_func_pointers(exe) | code_pointer_tables(exe)
             | overlay_data_func_pointers(exe, ov_dir))   # object-behavior handlers in overlay templates
    out_dir = os.path.dirname(out_path) or "."
    # Output is split into SHARDS translation units so the build compiles them in parallel.
    SHARDS = max(1, int(os.environ.get("PSXPORT_SHARDS", "8")))
    # NOTE: --overlays is NOT passed to the MAIN module's emit_module anymore (no `ov_dir`). The old
    # overlay_funcs() seeding (resident fns the overlays jal into) is still useful, so keep it:
    # Mid-function re-entry seeds — an entry inside another function whose body must fall through
    # into the seed (not `return`). Currently just the yield-primitive resume point 0x80051FA4.
    MAIN_REENTRY = {0x80051FA4}
    src_files = emit_module(exe, out_dir, MAIN_NAMES, seeds, ov_dir, limit, SHARDS,
                            reentry=MAIN_REENTRY)

    # The disc's boot stub (SCUS_944.54): the real PSX entry — draws SCEA, then LoadExec's MAIN.
    # It overlaps MAIN.EXE's address space, so it is emitted as a SEPARATE module (STUB_NAMES) with
    # its own dispatch/override symbols (stub_dispatch/stub_set_override) — see native_stub.c. Its
    # only seed is its entry; discovery follows the stub's own jal graph. (NOT linked today —
    # native_stub.cpp renders SCEA natively — so it stays out of the source manifest.)
    if "--stub" in sys.argv:
        stub = psexe.load(sys.argv[sys.argv.index("--stub") + 1])
        emit_module(stub, out_dir, STUB_NAMES, {stub.entry}, None, None, shards=2)

    # ---- OVERLAYS (two stacked stage slots) -----------------------------------------------------
    # The \BIN\*.BIN overlays are read RAW to a FIXED per-overlay base and run in place. They OVERLAP:
    # a given guest address is different code depending on which overlay is resident, so each is its OWN
    # module (ov_<tag>_*) keyed at its base+offset, and the runtime router (overlay_router.cpp) routes a
    # slot address to the CURRENTLY resident overlay (identified by a content signature of guest RAM at
    # the base). Bases below are the EXACT load destinations observed in the CD load-log (PSXPORT_DEBUG=cd
    # over a boot->field run); they are deterministic game facts, NOT magic offsets:
    #   STAGE slot 0x80106228 — START/DEMO/GAME.BIN (the main stage; mutually exclusive). cd-log:
    #     "loadfile 1648 B @LBA1904->0x80106228" (START), "5372 B @LBA1879" (DEMO), "11636 B @LBA1882" (GAME).
    #   MODE slot 0x80108F9C — SOP.BIN (the field/sub-mode overlay loaded RIGHT AFTER GAME.BIN, which stays
    #     resident at 0x80106228). cd-log: "async read 4415 words (17660 B) @LBA1895 -> 0x80108F9C";
    #     == engine_demo.cpp:445 "the overlay slot right after GAME.BIN". GAME [0x80106228,0x80108F9C) and
    #     SOP [0x80108F9C,..) are both resident together (adjacent, non-overlapping ranges).
    #   OPN.BIN — a sub-overlay loaded into the AREA slot 0x8018A000 (cd-log: "async read 13596 B @LBA1888
    #     -> 0x8018A000"; its header fn-ptrs (0x8018A348..) confirm that base). The big area DATA also loads
    #     to 0x8018A000 at other times; the resident-signature routing distinguishes them.
    #   CRD.BIN — the memory-CARD save browser (NOT "credits"): the title-menu Load-Game substate
    #     (DEMO sm[0x48]==4 -> FUN_8007BF20 -> FUN_8007BE18) and the in-game save UI. It loads into the
    #     SAME slot as OPN, 0x8018A000 — cd-log "async read 6265 words (25060 B) @LBA1866 -> 0x8018A000",
    #     captured by driving the title selection under PSXPORT_DEBUG=cd. Its base was previously guessed
    #     as the MODE slot, which put its bodies ~0x80080000 off: MAIN calls into it by hard-coded absolute
    #     (0x8018FA88 / 0x8018FBCC = CRD+0x5A88 / +0x5BCC, both `addiu sp,-0x20` prologues), so every such
    #     call fail-fasted as a rec_dispatch miss and the Load-Game browser aborted the process.
    #   A0*.BIN — the per-area FIELD CODE overlays (A00..A0L); each loads to the MODE slot 0x80108F9C
    #     (swapping out SOP), holding the field render submitters (0x8013xxxx). cd-log:
    #     "loadfile 285096 B @LBA374 -> 0x80108F9C" (A00). All A0* are interchangeable at this base.
    OVERLAY_BASES = {
        "START": 0x80106228, "DEMO": 0x80106228, "GAME": 0x80106228,
        "SOP": 0x80108F9C, "OPN": 0x8018A000, "CRD": 0x8018A000,
    }
    def overlay_base(stem):
        if stem in OVERLAY_BASES:
            return OVERLAY_BASES[stem]
        if re.fullmatch(r"A0[0-9A-Z]", stem):    # field area code overlays -> MODE slot
            return 0x80108F9C
        return None
    # Explicit per-overlay seeds: addresses reached NOT by a jal/pointer the scans see, but as a CLEAN
    # RE-ENTRY POINT the runtime resumes at — a "game-specific tailoring" (documented game fact). GAME's
    # cooperative task loop is re-entered each frame at its LOOP TOP 0x801063F4 (native_boot.cpp:285, the
    # scheduler sets resume PC = loop top after the GAME-stage SM yields). It is mid the task fn (the
    # prologue 0x8010637C is owned natively by ov_game_stage_main), so no scan finds it — seed it so the
    # per-frame resume dispatches to a recompiled body (the loop runs one frame then yields via ov_switch).
    OVERLAY_EXTRA_SEEDS = {
        # GAME loop top (per-frame re-entry) + the GAME stage-main 0x8010637C and DEMO stage-main
        # 0x801062E4. The stage-main entries are owned NATIVELY (ov_demo/ov_game dispatchers), so the
        # native path never dispatches them and no scan seeds them — but the FULL-PSX path (psx_fallback /
        # SBS gameplay/both) runs each stage as a pure recompiled body from its entry, so seed them.
        "GAME": {0x801063F4, 0x8010637C},
        "DEMO": {0x801062E4},
        # START.BIN is the stage-0 FILE-TABLE overlay: its header is a filename string table (count@base=6,
        # "\CD\SWDATA.BIN;1" ...), so NO scan finds code. The bootstrap FUN_800499e8 builds task0 with
        # entry PC = base+0x274 = 0x8010649C (a `addiu sp,-0x1c8` prologue, the START stage fn). The native
        # path owns that bootstrap (native_task0_bootstrap) so it never dispatches the recompiled body — but
        # the FULL-PSX path (PSXPORT_SBS_MODE=both core B, psx_fallback) runs task0 under the substrate and
        # resumes at 0x8010649C, so seed it (a documented runtime-computed re-entry, like GAME's loop top).
        "START": {0x8010649C},
        # SOP field-mode machine (FUN_80109450): the SOP.BIN overlay body dispatched from GAME.BIN
        # (caller ra 0x801088B0 in gameplay-mode SBS, full-PSX field mode). It's the sm[0x4e]-indexed
        # sub-mode dispatcher (per findings/render.md's later-286 note); the scan misses it because
        # GAME.BIN calls it by hard-coded absolute 0x80109450 into the SOP overlay slot, and the
        # cross-module discovery doesn't cross the resident->overlay boundary. Seed it so the full-PSX
        # path can advance past field entry (SBS gameplay/full mode, or PSXPORT_GATE=1 field runs).
        "SOP": {0x80109450},
        # The per-AREA handler tables (indexed by the area byte DAT_800BF870) are NOT listed here —
        # they are discovered from the binary by area_indexed_overlay_tables() below. Only the two
        # addresses that are NOT members of such a table stay explicit:
        # 0x801158E0 (2026-07-10): area-1 handler reached via a computed pointer from resident
        #   FUN_800263C0's dispatch (a0=node) — REPL `warp 1` under PSXPORT_GATE=1 hit a miss.
        "A01": {0x801158E0},
        # 0x80111A20 (2026-07-10, issue #33): attract-demo chain ov_a02 FUN_801122A4 dispatches it
        #   (caller ra=0x80111CAC); discovery missed it — un-driven title screen crashed the port.
        "A02": {0x80111A20},
    }
    # ---- pass 1: load every overlay image (the area set must be complete + IN AREA ORDER before
    # the per-area MAIN tables can be resolved — entry i of such a table is an address in overlay i).
    ov_images = []          # (stem, fn, base, data, ovexe)
    if ov_dir and os.path.isdir(ov_dir):
        for fn in sorted(os.listdir(ov_dir)):
            if not fn.upper().endswith(".BIN"):
                continue
            data = open(os.path.join(ov_dir, fn), "rb").read()
            data = data[:len(data) & ~3]      # align to a word; trailing 1-3 padding bytes aren't code
            stem = fn[:-4].upper()
            base = overlay_base(stem)
            if base is None:
                print(f"[overlays] WARNING: {fn} has no known load base — defaulting to 0x80106228; "
                      f"capture its real dest with PSXPORT_DEBUG=cd and add it to OVERLAY_BASES")
                base = 0x80106228
            ov_images.append((stem, fn, base, data,
                              psexe.PsxExe(base, 0, base, len(data), 0, 0, data)))
    # A0<n> sorts lexicographically in area order (A00..A09, A0A..A0L), which is the index order the
    # game's area byte uses; sorted(os.listdir) already put them that way.
    area_exes = [(stem, ovexe) for stem, _, _, _, ovexe in ov_images
                 if re.fullmatch(r"A0[0-9A-Z]", stem)]
    area_tbl_seeds = area_indexed_overlay_tables(exe, area_exes)

    # ---- pass 2: emit each overlay module.
    overlays = []   # (tag, NAME, base, end, sig32 bytes, Names)
    for stem, fn, base, data, ovexe in ov_images:
        tag = re.sub(r"[^a-z0-9]", "_", fn[:-4].lower())
        N = overlay_names(tag)
        # Both the explicit seeds and the discovered per-area table entries are runtime-computed
        # ENTRY POINTS, so they get the same re-entry treatment (a preceding body that runs off its
        # end into one of them must continue into it, not `return`).
        explicit = OVERLAY_EXTRA_SEEDS.get(stem, set()) | area_tbl_seeds.get(stem, set())
        hard_ov = (pointer_table_funcs(ovexe) | constructed_func_pointers(ovexe)
                   | code_pointer_tables(ovexe) | overlay_internal_jal_targets(ovexe)
                   | explicit)
        soft_ov = func_entries_after_return(ovexe)   # jr-ra boundaries (mergeable if false)
        src_files += emit_module(ovexe, out_dir, N, hard_ov, None, None, shards=2,
                                 soft_seeds=soft_ov, reentry=explicit)
        overlays.append((tag, fn[:-4].upper(), base, base + len(data), data[:32], N))

    # Overlay routing table consumed by overlay_router.cpp.
    write_overlay_table(out_dir, exe, overlays)
    src_files.append("overlay_table.c")

    # Source manifest: cmake reads generated/rec_sources.cmake to link exactly the emitted TUs (the
    # overlay set is dynamic). Deterministic — no globbing (which would wrongly pull the unlinked
    # stub TUs and collide with dispatch.cpp's stub_dispatch).
    manifest = "set(GEN_REC_SRCS\n  " + "\n  ".join(sorted(set(src_files))) + ")\n"
    open(os.path.join(out_dir, "rec_sources.cmake"), "w").write(manifest)

    # Stub the old monolith path so a stale copy is never compiled.
    open(out_path, "w").write("// recompiled core is split into shard_*.c — see rec_decls.h\n")

    # Stamp the recomp version into the output so ensure_recomp.py can detect a stale generated/ produced
    # by an older recompiler (the explicit, machine-independent staleness signal — see RECOMP_VERSION).
    open(os.path.join(out_dir, ".recomp_version"), "w").write(RECOMP_VERSION + "\n")
    print(f"[recomp] version {RECOMP_VERSION}")


def overlay_internal_jal_targets(exe):
    """Direct-jal targets that land WITHIN this overlay (seed them so discover_funcs follows the
    overlay's internal call graph). jals to MAIN resident addresses are outside [load,text_end) and
    routed through the global router at runtime, not seeded here."""
    lo, hi = exe.load, exe.text_end
    return {ins.target for a in range(lo, hi, 4)
            for ins in (decode(a, exe.word(a)),)
            if ins.op == "jal" and lo <= ins.target < hi}


def area_indexed_overlay_tables(main_exe, area_exes, min_valid_frac=0.8):
    """Seed the per-area handler tables that live in RESIDENT MAIN's data but point into the
    SWAPPABLE overlay slot.

    The game keeps several parallel dispatch tables indexed by the area byte (0x800BF870):
    `(&TABLE)[area](args)`. Each table has exactly one entry per area overlay, and entry *i* is an
    address inside overlay *i*'s image — a different function in a different file for every index.
    Neither per-module scan can see them: MAIN's `pointer_table_funcs`/`code_pointer_tables` reject
    the words (they point outside MAIN's text), and each overlay's own scan never looks at MAIN's
    data. So every one of these entries is invisible to discovery and fail-fasts as a
    `rec_dispatch` miss the first time the area that owns it is entered.

    The tables identify themselves without any hand-maintained address list: a window of N
    consecutive image words (N = number of area overlays) where word *i* is word-aligned and inside
    overlay *i*'s image, and the overwhelming majority of the words are a valid FUNCTION ENTRY in
    their OWN overlay. The per-index bounds test is the discriminator — the overlays differ in size
    by more than 10x, so a coincidental run of overlay-range words essentially never satisfies it
    for every index. Only entries that really are function entries in their own overlay are seeded
    (a table may carry a stale/duplicated slot for an area that never calls it — seeding that would
    split a real function, the exact failure c0caeef2 fixed).

    `area_exes` is the ORDERED list of (stem, PsxExe) for the area overlays, in area order.
    Returns {stem: {addr, ...}}.
    """
    n = len(area_exes)
    out = {stem: set() for stem, _ in area_exes}
    if n < 2:
        return out
    def entry_ok(ex, w):
        # is_func_entry's two signals, plus the NULL HANDLER: a bare `jr ra` at a call-table target is
        # a do-nothing handler function, and nothing else it could be. It matches neither signal — no
        # `addiu sp` frame, and it is typically laid out right after a data island (A0L's 0x80109200
        # follows a pointer table), so it is not preceded by another function's `jr ra` either.
        return is_func_entry(ex, w) or ex.word(w) == 0x03E00008

    lo, hi = main_exe.load, main_exe.text_end
    need = int(round(n * min_valid_frac))
    found = 0
    a = lo
    while a + n * 4 <= hi:
        words = [main_exe.word(a + i * 4) for i in range(n)]
        inb = all((w & 3) == 0 and ex.load <= w < ex.text_end - 4
                  for w, (_, ex) in zip(words, area_exes))
        if not inb:
            a += 4
            continue
        entries = [entry_ok(ex, w) for w, (_, ex) in zip(words, area_exes)]
        if sum(entries) >= need:
            for w, ok, (stem, _) in zip(words, entries, area_exes):
                if ok:
                    out[stem].add(w)
            found += 1
            print(f"[overlays] area-indexed table @0x{a:08X}: {sum(entries)}/{n} entries seeded")
            a += n * 4
            continue
        a += 4
    if found:
        print(f"[overlays] {found} area-indexed handler table(s) in MAIN -> "
              f"{sum(len(v) for v in out.values())} overlay seeds")
    return out


def write_overlay_table(out_dir, main_exe, overlays):
    """Emit generated/overlay_table.{h,c}: MAIN text range + per-overlay (base,end,name,dispatch,sig)
    descriptors for the runtime router (overlay_router.cpp)."""
    mlo, mhi = main_exe.load & 0x1FFFFFFF, main_exe.text_end & 0x1FFFFFFF
    h = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", "#pragma once",
         '#include "core.h"',
         '#include "recomp_iface.h"   // framework: struct RecOverlay (owned framework-side, not redefined here)', "",
         f"#define REC_MAIN_LO 0x{mlo:08X}u", f"#define REC_MAIN_HI 0x{mhi:08X}u", "",
         "void main_dispatch(Core*, uint32_t);", ""]
    for tag, NAME, base, end, sig, N in overlays:
        h.append(f"void {N.dispatch}(Core*, uint32_t);")
        h.append(f"int {N.index}(uint32_t);")
    h += ["extern const RecOverlay g_rec_overlays[];", "extern const int g_rec_overlay_count;", ""]
    open(os.path.join(out_dir, "overlay_table.h"), "w").write("\n".join(h) + "\n")

    c = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", '#include "overlay_table.h"', ""]
    for tag, NAME, base, end, sig, N in overlays:
        bytes_c = ", ".join(f"0x{b:02X}" for b in sig)
        c.append(f"static const unsigned char sig_{tag}[{len(sig)}] = {{ {bytes_c} }};")
    c.append("")
    c.append("const RecOverlay g_rec_overlays[] = {")
    for tag, NAME, base, end, sig, N in overlays:
        c.append(f"  {{ 0x{base:08X}u, 0x{end:08X}u, \"{NAME}\", {N.dispatch}, {N.index}, "
                 f"sig_{tag}, {len(sig)} }},")
    c.append("};")
    c.append(f"const int g_rec_overlay_count = {len(overlays)};")
    open(os.path.join(out_dir, "overlay_table.c"), "w").write("\n".join(c) + "\n")
    print(f"[overlays] {len(overlays)} overlay module(s) -> overlay_table.c")


if __name__ == "__main__":
    main()
