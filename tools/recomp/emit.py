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
import os, sys, re, json
sys.path.insert(0, os.path.dirname(__file__))
import psexe
import decode as D
from decode import decode


# ---- game-supplied recompiler seeds (--seeds) --------------------------------------------------
# The seed set is a GAME FACT, not a framework fact: which addresses are reached only indirectly
# (fn-pointer / runtime-computed re-entry) is a property of the specific executable, and the load
# base of each overlay is a property of the specific disc. Baking one game's addresses into the
# recompiler is not merely untidy — it is WRONG for every other game: a foreign seed that lands
# inside the new game's text SPLITS a real function at an arbitrary offset (silently corrupting the
# recomp), and one that lands outside it raises on decode. So a consumer supplies its own seed file
# and the framework ships none. See docs/porting-a-new-psx-game.md.
def load_seeds(path):
    """Parse a game's seed file. JSON with `//` line comments allowed — the RATIONALE for each seed
    (how it is reached, which run surfaced it) is the most valuable part of the file and must stay
    next to the address, so plain JSON is not enough.

    Schema (every key optional):
      main                  [addr, ...]          resident-module seeds
      main_reentry          [addr, ...]          mid-function re-entry points in the resident module
      overlay_bases         {STEM: addr, ...}    per-overlay load base
      overlay_base_patterns [[regex, addr], ...] load base for overlay stems matching a regex
      overlay_seeds         {STEM: [addr, ...]}  per-overlay explicit seeds
    Addresses are hex strings ("0x80051FA4") or ints.
    """
    with open(path) as f:
        raw = f.read()
    # Strip `//` comments outside of string literals (a naive replace would corrupt any string
    # containing a slash, e.g. a disc path like "\CD\SWDATA.BIN;1" in a rationale comment).
    out, i, in_str, esc = [], 0, False, False
    while i < len(raw):
        ch = raw[i]
        if in_str:
            out.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < len(raw) and raw[i + 1] == "/":
            while i < len(raw) and raw[i] != "\n":
                i += 1
            continue
        out.append(ch)
        i += 1
    try:
        data = json.loads("".join(out))
    except json.JSONDecodeError as e:
        sys.exit(f"[recomp] {path}: malformed seed file: {e}")

    def addr(v, where):
        if isinstance(v, int):
            return v
        try:
            return int(str(v), 16 if str(v).lower().startswith("0x") else 10)
        except ValueError:
            sys.exit(f"[recomp] {path}: {where}: not an address: {v!r}")

    def addrs(vs, where):
        return {addr(v, where) for v in vs}

    known = {"main", "main_reentry", "overlay_bases", "overlay_base_patterns", "overlay_seeds"}
    unknown = set(data) - known
    if unknown:
        sys.exit(f"[recomp] {path}: unknown key(s) {sorted(unknown)}; expected {sorted(known)}")
    return {
        "main": addrs(data.get("main", []), "main"),
        "main_reentry": addrs(data.get("main_reentry", []), "main_reentry"),
        "overlay_bases": {k: addr(v, f"overlay_bases[{k}]")
                          for k, v in data.get("overlay_bases", {}).items()},
        "overlay_base_patterns": [(rx, addr(v, "overlay_base_patterns"))
                                  for rx, v in data.get("overlay_base_patterns", [])],
        "overlay_seeds": {k: addrs(v, f"overlay_seeds[{k}]")
                          for k, v in data.get("overlay_seeds", {}).items()},
    }


EMPTY_SEEDS = {"main": set(), "main_reentry": set(), "overlay_bases": {},
               "overlay_base_patterns": [], "overlay_seeds": {}}


def check_seeds_in_text(exe, seeds, where):
    """Fail with a READABLE diagnostic when a seed lies outside the module's text.

    Without this the failure is an IndexError from decode() deep inside discovery, which reads as a
    recompiler bug rather than a bad seed file. This is exactly how a foreign game's seed list
    announces itself, so the message names the likely cause."""
    bad = sorted(a for a in seeds if not (exe.load <= a < exe.text_end - 4))
    if bad:
        sys.exit(f"[recomp] {where}: seed(s) outside the module text "
                 f"[0x{exe.load:08X},0x{exe.text_end:08X}): "
                 + ", ".join(f"0x{a:08X}" for a in bad)
                 + "\n  A seed must be an address INSIDE the image being recompiled. If these are "
                   "another game's addresses, point --seeds at this game's own seed file.")

# Recomp version stamp — BUMP this whenever the recompiler logic or the seed set changes in a way that
# must invalidate every machine's existing generated/ output. tools/ensure_recomp.py folds it into the
# recomp identity and re-emits when the stamp on disk (generated/.recomp_version) differs, so a stale
# generated/ on another box (which an input-content hash alone failed to catch — a box can build a
# self-consistent-but-outdated set) is forced to regenerate. Date + a per-day counter; keep it terse.
# GTE data registers holding a PROJECTED SCREEN XY. DR12/13/14 are the XY_FIFO slots an RTPT fills
# (v0,v1,v2); DR15 is the slot an RTPS fills for a single vertex. A `swc2` of one of these is a
# packet-vertex store — see the GTE_STORE case in emit_simple. They pair with the Z_FIFO registers
# DR17/18/19 (DR15 pairs with DR19, the same slot RTPS writes).
GTE_SCREEN_XY_REGS = (12, 13, 14, 15)

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
        # THE NATIVE-DEPTH SEAM, and it is generic — no per-game RE. A `swc2` of a GTE SCREEN-XY
        # register (DR12/13/14, or DR15 for the RTPS single-vertex slot) IS the packet-vertex store:
        # `gte_stsxy*` compiles to exactly this. The address it writes is the key the renderer later
        # looks up in gp0_exec to recover that vertex's view-space Z, so tapping here gives every game
        # real per-vertex depth without reverse-engineering its submit path.
        # `rt` is a compile-time constant here, so every other cop2 store keeps the plain form and the
        # tap costs nothing on them.
        if ins.rt in GTE_SCREEN_XY_REGS:
            return f"gte_store_xy(c, {addr_expr(ins)}, {ins.rt});"
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
            # THIRD IDIOM — computed OFFSET, no table at all (Spyro's GTE code):
            #     lui/addiu rB, <immediate base>   ; sll rI, rI, k   ; add rD, rB, rI ; jr rD
            # target = base + idx*2^k, i.e. a jump INTO AN UNROLLED RUN of fixed-size blocks. Both
            # idioms above require the target ADDRESS to be read from a table with `lw rN,OFF(base)`;
            # here there is no table and nothing to read, so they return None and the jr would fall
            # through to rec_dispatch and fail-fast at runtime, one case per build+run cycle.
            #
            # Runs LAST, only when both table idioms found nothing, so an already-correct recovery can
            # never be perturbed — the same strict-first discipline the table idioms use on each other.
            #
            # The case COUNT is not guessed. This idiom's index is assigned CONSTANTS in a branch
            # cascade dominating the jr (Spyro: s1 <- 0 / 1 / 2 at three sites), so the reachable case
            # set is exactly the set of those constants. If no constant assignment is found, we emit
            # NOTHING rather than assume a range — a wrong case label carves up real code.
            res2 = _scan_computed_offset(ins, a, i.rs, lo, hi)
            if res2 is None:
                continue
            jt[a] = res2
            continue
        tbl, count, targets = res
        if tbl_spans is not None:                  # the DATA table occupies [tbl, tbl+4*count)
            tbl_spans.add((tbl, count))
        jt[a] = targets
    return jt



class _NoIns:
    kind = None


_N = _NoIns()


def _scan_computed_offset(ins, jr_a, dst, lo, hi, window=160):
    """Recover `jr base + idx*2^k` — a computed OFFSET dispatch with NO address table.

    Shape:  lui/addiu rB,<immediate base> ; sll rI,rI,k ; add dst,rB,rI ; jr dst

    The two idioms above this one both require the target ADDRESS to be read from a table with
    `lw rN,OFF(base)`. Here there is no table: the base is an immediate and the index is scaled into an
    offset, so the jump lands in an UNROLLED RUN of equal-sized blocks. Returns the case targets, or
    None. Emitting nothing is always preferable to guessing — but note these become LABELS inside the
    enclosing function, not function entries, so an extra label is dead code the C compiler drops while
    a MISSING one is a runtime fail-fast. Coverage is therefore biased generous, deliberately.

    The window is wide because consecutive dispatchers SHARE one `sll` (three in a row here reuse an
    `sll s1,s1,4` sitting ~68 instructions back), so a tight window silently matches only the first.
    """
    # 1. the `add dst, rB, rI` feeding the jr
    add_a = srcs = None
    for a in range(jr_a - 4, max(lo, jr_a - window * 4) - 4, -4):
        i = ins.get(a)
        if i is None:
            continue
        if i.kind == D.ALU_RRR and i.op in ("add", "addu") and i.rd == dst:
            add_a, srcs = a, (i.rs, i.rt)
            break
        if (i.rd == dst or i.rt == dst) and i.op != "sll":
            # dst rewritten by something that is not the base+index `add`. If that write is IMMEDIATE
            # construction (lui/addiu), this is the no-index form and the constants below are the
            # answer — bailing here made the scan blind to it, because the very instruction that
            # assigns the target address looked like disqualifying interference.
            if i.op in ("lui", "addi", "addiu"):
                break
            return None
    if add_a is None:
        # NO INDEX AT ALL: `lui rX,HI ; addiu rX,rX,LO ; ... ; jr rX` — an indirect jump whose register
        # holds a plain IMMEDIATE, set at one or more sites in the function (Spyro's 0x800243FC reaches
        # 0x800240F8 or 0x800241A8 this way). There is no base+index to decompose, so the earlier scan
        # bails; the targets are simply the constants assigned to that register. Same principle as the
        # constant-index route — read the values the code actually puts there — applied one level up.
        consts, hi_v = set(), {}
        for a in range(lo, hi, 4):
            i = ins.get(a)
            if i is None:
                continue
            if i.op == "lui":
                hi_v[i.rt] = i.imm << 16
            elif i.op in ("addi", "addiu") and i.rs in hi_v:
                v = (hi_v[i.rs] + i.simm) & 0xFFFFFFFF
                hi_v[i.rt] = v
                if i.rt == dst and lo <= v < hi:
                    consts.add(v)
        return sorted(consts) if len(consts) >= 2 else None

    # 2. the immediate base. FORWARD, because `lui rB,HI ; addiu rB,rB,LO` cannot be resolved walking
    #    backward — the addiu is met before the lui that gives it meaning.
    base = base_reg = None
    hi_val = {}
    for a in range(max(lo, add_a - window * 4), add_a, 4):
        i = ins.get(a)
        if i is None:
            continue
        if i.op == "lui":
            hi_val[i.rt] = i.imm << 16
        elif i.op == "addiu" and i.rs in hi_val and i.rt in srcs:
            base, base_reg = (hi_val[i.rs] + i.simm) & 0xFFFFFFFF, i.rt
    if base_reg is None or not (lo <= base < hi):
        return None

    # 3. the scale. The add's operands ARE the base and the scaled index, so the `sll` must write the
    #    OTHER addend — keying on "any sll writing either operand" picked up an unrelated `sll` on the
    #    base register once the window widened, giving idx == base and no cases.
    other = srcs[1] if srcs[0] == base_reg else srcs[0]
    idx_reg = shift = scale_op = None
    for a in range(max(lo, add_a - window * 4), add_a, 4):
        i = ins.get(a)
        # `srl` as well as `sll`: the index is sometimes a BITFIELD, masked out with andi and shifted
        # DOWN into place (0x8004E5A4-0x8004E5A8 is `andi t2,v0,0xFF00 ; srl t2,t2,5`, i.e. bits 8-15
        # scaled by 8). An sll-only match missed that whole family.
        if i is not None and i.op in ("sll", "srl") and i.rd == other:
            idx_reg, shift, scale_op = i.rt, i.shamt, i.op
    if idx_reg is None:
        return None
    # For a right-shifted bitfield the constant-index route is meaningless (the constants feeding it are
    # pre-shift field values), so the run walk is the only sound bound — it is unioned in below anyway.
    stride = (1 << shift) if scale_op == "sll" else 8

    targets = set()
    # 4a. indices assigned as CONSTANTS by the branch cascade dominating the jr — exact when the index
    #     is static.
    for a in range(lo, jr_a, 4):
        i = ins.get(a)
        if (scale_op == "sll" and i is not None and i.op in ("addi", "addiu")
                and i.rs == 0 and i.rt == idx_reg and i.simm >= 0):
            targets.add((base + (i.simm << shift)) & 0xFFFFFFFF)
    # 4b. the RUN's own extent — needed when the index is dynamic (a counter, a masked field), and
    #     unioned rather than used as a fallback: an index that is only PARTLY static yields one
    #     constant and would otherwise leave the rest of its run unreachable.
    # Walk at the recovered stride AND at the minimum 8. A dispatcher's `sll` scale does not always
    # match the spacing of the run it lands in — 0x8004C818 scales by 16 while its trampolines sit 8
    # apart, so the stride-16 walk stepped straight over half of them and 0x8004C828 stayed a fail-fast.
    # Unioning a stride-8 walk costs only extra labels (dead code the compiler drops) and removes a
    # whole class of near-misses.
    for stride in sorted({stride, 8} if stride >= 8 else {8}):
        miss = 0
        for k in range(256):
            blk = (base + k * stride) & 0xFFFFFFFF
            if not (lo <= blk < hi) or blk + stride > hi:
                break
            if any((ins.get(blk + o) or _N).kind in (D.JUMP, D.JUMPR) for o in range(0, stride, 4)):
                targets.add(blk)
                miss = 0
            else:
                # Tolerate a gap rather than stopping dead: the base can point at PADDING before the
                # run proper (0x8004C620 is two nops, with the first real case at index 1), so breaking
                # on the first jumpless block found nothing at all there. Two in a row is the end.
                miss += 1
                if miss >= 2:
                    break

    out = sorted(t for t in targets if lo <= t < hi)
    return out if len(out) >= 2 else None


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


# Native-depth tap counters, summed across every function emitted in a module and reported once by
# emit_module. A silent tap is the failure mode this whole feature has to avoid — "0 recorded" and "no
# 3D content" look identical at runtime — so the BUILD says how many stores it found.
g_pz_tapped = 0
g_pz_skipped = 0


def defines_reg(i, r):
    """Does instruction `i` write GPR `r`? Used by vertex_pz_stores to know when a projected vertex
    value has been overwritten. Deliberately OVER-approximates: anything not clearly a non-writer is
    treated as a write, so an unrecognised instruction breaks the pairing rather than silently
    carrying a stale association past it. A missed vertex costs depth on one primitive; a wrongly
    paired one draws real geometry at the wrong distance."""
    k = i.kind
    if k in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V):
        return i.rd == r
    if k in (D.ALU_RRI, D.LUI, D.LOAD, D.GTE_MOVE) and i.op in (
            "addi", "addiu", "andi", "ori", "xori", "slti", "sltiu", "lui",
            "lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr", "mfc2", "cfc2"):
        return i.rt == r
    if k == D.HILO and i.op in ("mfhi", "mflo"):
        return i.rd == r
    if k == D.COP0 and i.op == "mfc0":
        return i.rt == r
    # Non-writers: stores, the COP2 ops and cop2-side moves/loads, mult/div, nop.
    # GTE_OP and GTE_LOAD (lwc2) write the COP2 register file, never a GPR — leaving them in the
    # "unknown, assume it writes" bucket silently killed every software-pipelined pairing, which is
    # the majority of the real ones.
    if k in (D.STORE, D.GTE_STORE, D.GTE_LOAD, D.GTE_OP, D.MULDIV, D.NOP):
        return False
    if k == D.GTE_MOVE:            # mtc2 / ctc2 write the COP2 side, not a GPR
        return False
    if k == D.HILO:                # mthi / mtlo
        return False
    return True                    # unknown: assume it writes, and stop the pairing


def vertex_pz_stores(ins, lo, hi):
    """{sw_addr: paired_Z_register} for the `mfc2 rX, DR12..15` -> `sw rX, off(base)` vertex idiom.

    THE SECOND HALF OF THE NATIVE-DEPTH TAP. `swc2` of a screen-XY register is one way a game writes a
    projected vertex into its primitive packet (see GTE_SCREEN_XY_REGS in emit_simple); the other, just
    as common, is to move the value into a GPR with `mfc2` and store it with a plain `sw`. A tap that
    only understands `swc2` records nothing at all for such a game, and records it SILENTLY — "no
    depth" and "no 3D content" look identical. Spyro's main executable is exactly this shape: zero
    `swc2` of DR12-15, and 70 `mfc2` of them (issue 0036).

    The scan is deliberately LOCAL and conservative. Walking forward from each `mfc2 rX, DRk` it stops
    at the first of:

      * a redefinition of rX          — the word finally stored is no longer that vertex,
      * any control transfer          — the store is no longer unconditionally reached from the mfc2.

    Both are correctness requirements, not caution: pairing across either attaches a WRONG depth to a
    real vertex, which is worse than attaching none (a wrong depth draws geometry at the wrong
    distance; a missing one falls back to draw order, which is what we have today).

    AN INTERVENING COP2 OP IS NOT A BARRIER, and treating it as one threw away most of the real sites.
    PSX submit loops are SOFTWARE PIPELINED: they read vertex N's screen XY, issue the transform for
    vertex N+1, and only then store N — so by the time the store runs, the Z FIFO holds N+1's depth.
    Reading Z at the store is what would be wrong. Instead the Z is HELD at the mfc2, the one moment it
    is still this vertex's, and consumed at the store. In Spyro this is 29 of 70 sites — the majority
    of the ones that matter.

    A store sitting in a BRANCH DELAY SLOT is skipped, because the emitter embeds delay slots inside
    the control statement and there is nowhere to append the record call. Those are counted and
    reported by the caller rather than dropped quietly."""
    # DR12/13/14 are the XY_FIFO slots an RTPT fills; DR15 is RTPS's single-vertex slot. Z pairs:
    # 12->17, 13->18, 14->19, and 15->19 (RTPS writes the same Z slot).
    ZPAIR = {12: 17, 13: 18, 14: 19, 15: 19}
    holds, out, skipped_ds = {}, {}, 0
    # Delay-slot addresses: the word after any control op is emitted inside that op's statement.
    ds = {a + 4 for a in range(lo, hi, 4)
          if a in ins and ins[a].kind in (D.BRANCH, D.JUMP, D.JUMPR)}
    for a in range(lo, hi, 4):
        i = ins.get(a)
        if i is None or i.kind != D.GTE_MOVE or i.op != "mfc2" or i.rd not in ZPAIR:
            continue
        gpr = i.rt
        if gpr == 0:
            continue                                  # mfc2 into $zero: discarded, not a vertex
        b = a + 4
        while b < hi:
            j = ins.get(b)
            if j is None:
                break
            if j.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                break                                 # block boundary: store not unconditionally reached
            if j.kind == D.STORE and j.op == "sw" and j.rt == gpr:
                if b in ds:
                    skipped_ds += 1
                else:
                    holds[a] = (gpr, ZPAIR[i.rd])     # hold the Z at the mfc2 …
                    out[b] = gpr                      # … and consume it at the store
                break
            if defines_reg(j, gpr):
                break                                 # the vertex value is gone
            b += 4
    return holds, out, skipped_ds


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
    contiguous run; fall target set when the run flows back into [lo,hi) (needs a goto).

    JALR IS A CALL, NOT A TERMINATOR. `jr` and `jalr` share the JUMPR kind, and treating them alike here
    is wrong in a way that produces no error and no crash — only a wrong register hundreds of thousands of
    instructions later. `jalr rd, rs` writes rd (= ra) with the address of the instruction after the delay
    slot, so control COMES BACK to it; `jr rs` does not. Stopping the walk at a `jalr` truncates the run
    just before that instruction, and the emitter then closes the run with a bare `return;` — silently
    dropping everything after the call.

    That is not hypothetical. Spyro 0x80047B60 is a 45-case state machine whose cases converge on
    `jalr v0` at 0x800487D8 with the function's own epilogue at 0x800487E0, the very next instruction:

        800487d8: jalr v0            ; ra = 0x800487E0
        800487dc: nop                ;   delay slot
        800487e0: ...                ; <- the epilogue: lw ra/s2/s1/s0 from the frame, addiu sp,+144, jr ra

    The truncated run emitted the call and returned, so the epilogue never ran: `s0` came back holding the
    case body's `addiu s0, sp, 32` instead of the caller's saved 0. The caller at 0x80048C04 uses `s0` as a
    loop counter tested against a count of 2 — with a large positive value in it the loop ran ~62 MILLION
    times and the port stopped presenting frames entirely (Spyro issue 0034). Nothing about the symptom
    pointed here; it took an ABI check around a traced call to name the function."""
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
                elif i.op == "jalr":
                    a = ds + 4       # see JALR IS A CALL below
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
                    elif i.op in ("jal", "jalr"):   # both are CALLS — see JALR IS A CALL below
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
    ra_tails = ra_tail_returns(ins, lo, hi)
    # Native depth, mfc2 form: which `sw` stores a projected vertex, and which Z reg pairs with it.
    pz_holds, pz_stores, pz_skipped_ds = vertex_pz_stores(ins, lo, hi)
    global g_pz_tapped, g_pz_skipped
    g_pz_tapped += len(pz_stores); g_pz_skipped += pz_skipped_ds
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
    # BACK-EDGE targets: a branch/jump whose target is at or below its own address closes a LOOP.
    #
    # These are where the deferred-work gate has to be re-tested. The gate is otherwise only at
    # function ENTRY, which is a fine boundary for a function that calls something — but a guest loop
    # that calls NOTHING never reaches another entry, so the host can never take a turn while it
    # spins. That is not hypothetical: Spider-Man's boot has two such loops, `while (counter < target)`
    # in its field-wait primitive and `do {} while (flag)` in its own main(), and each one wedged the
    # port until it was owned natively one at a time. Gating back-edges fixes the whole class instead.
    #
    # Only back-edges, not every label: a forward branch cannot spin, so gating it would cost the hot
    # path with no benefit. A self-loop (target == own address) counts, hence <=.
    backedges = {i.target for x, i in ins.items()
                 if i.kind in (D.BRANCH, D.JUMP) and i.target is not None
                 and i.target in ins and i.target <= x}
    backedges &= labels
    #
    # ON by default. It was briefly disabled on the belief that it corrupted guest state; that was
    # WRONG and the retraction is worth keeping, because the reasoning error is easy to repeat.
    #
    # The A/B was real: enabling the gate produced a corrupt global-base register ($fp) in the
    # Spider-Man port, disabling it did not. But the gate was not the corruptor. It only lets the host
    # take a turn inside call-free spin loops, which is what finally let that boot RUN FAR ENOUGH to
    # consume an $fp that a branch-and-link mistranslation had already wrecked (see the
    # BRANCH-AND-LINK note in emit_control). Fixing that mistranslation makes the same A/B come out
    # clean, with the gate on. A reproducible A/B proves correlation with the flag, not that the flag
    # is the cause — the flag can simply be what makes an existing bug reachable.
    #
    # Two other hypotheses were falsified along the way and should not be re-tried: register
    # save/restore across the poll (a PSXPORT_DEBUG=pollregs probe reports zero clobbers of
    # sp/fp/gp/s0-s7 over a full boot), and the host turn ignoring guest critical sections.
    # MEASUREMENT ONLY (PSXPORT_LABEL_ALL=1): label every standalone instruction, so any address inside
    # a recompiled function could be resumed at. This is the cost half of docs/issues/0021's option 1 —
    # it does NOT wire up mid-function ENTRY (that needs a dispatch switch at the top of each function),
    # it only answers "what do universal labels cost in size and build time". Not for shipping.
    if os.environ.get("PSXPORT_LABEL_ALL") == "1":
        labels = set(standalone)
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
                # Loop back-edge: give the host a turn. One predictable load-and-test per iteration,
                # and only on loops — see the `backedges` note above for why function entry alone is
                # not enough.
                if a in backedges:
                    out.append("  if (c->pending_work) rec_irq_poll(c);")
            if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                slot = ins.get(a + 4)
                ds_c = emit_simple(slot) if (slot and slot.kind not in
                       (D.BRANCH, D.JUMP, D.JUMPR)) else "/* DS */"
                out.extend(emit_control(i, ds_c, funcset, labels, N, jt.get(a), a in ra_tails))
                if (a + 4) in ds_label_targets:        # the delay slot is also a branch target
                    out.append(f"  goto L_DSAFTER_{a:08X};")
                    out.append(f"L_{a + 4:08X}:; {ds_c}")
                    out.append(f"L_DSAFTER_{a:08X}:;")
                a += 8
            else:
                st = emit_simple(i)
                if st:
                    out.append("  " + st)
                # NATIVE DEPTH (mfc2 form): this `sw` wrote a projected vertex's screen XY. Record its
                # view-space Z against the address just written, which is the key gp0_exec looks up.
                # Emitted AFTER the store so the memory write is unchanged and the tap is purely additive.
                if a in pz_holds:
                    g, z = pz_holds[a]
                    out.append(f"  gte_hold_pz(c, {g}, {z});")
                if a in pz_stores:
                    out.append(f"  gte_record_pz(c, {addr_expr(i)}, {pz_stores[a]});")
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
        if last.kind == D.JUMPR and last.op == "jr":   # jr ra / computed jump — no fall-through
            return False
        # a trailing `jalr` DOES fall through (it is a call; ra points at `hi`) — see JALR IS A CALL
        # in collect_tail_dups.
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



def ra_tail_returns(ins, lo, hi):
    """Addresses of `jr rX` (rX != ra) where rX provably holds `ra` — i.e. TAIL-RETURNS, not calls.

    A guest that stashes `ra` in another register and jumps through it is returning. The recompiler
    otherwise routes that to the dispatcher, where the target — the instruction after some caller's
    `jal` — is mid-function, never a function entry, and fail-fasts. Deciding it here is static and
    conservative: only a register whose value provably came from `ra` inside this function qualifies.

    Both move forms matter. The R-type (`addu rX,ra,zero`) is the obvious one; the I-type
    (`addi rX,ra,0`) is what the real case uses, and a check that only looks at `rd` misses it
    silently — which first told me the pattern occurred once in the whole game instead of four times.
    Also covers `sw ra,N(sp)` followed by `lw rX,N(sp)`.
    """
    out, slots, src = set(), set(), {}
    for a in range(lo, hi, 4):
        i = ins.get(a)
        if i is None:
            continue
        if i.op == "sw" and i.rt == 31:
            slots.add(i.simm)
        elif i.kind == D.ALU_RRR and 31 in (i.rs, i.rt):
            src[i.rd] = True
        elif i.op in ("addi", "addiu") and i.rs == 31:
            src[i.rt] = True
        elif i.op == "lw" and i.simm in slots:
            src[i.rt] = True
        elif i.op == "jr" and i.rs != 31 and src.get(i.rs):
            out.add(a)
        elif i.rd and i.kind in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V):
            src.pop(i.rd, None)          # clobbered by something unrelated to ra
    return out


def emit_control(i, ds_c, funcset, labels, N, jtargets=None, ra_tail=False):
    """Lines for a control instruction `i` whose delay-slot C is `ds_c`. `jtargets` (if set) = the
    recovered jump-table case-label addresses for a computed `jr` -> emit a C switch on the target
    value (auto-dedupes repeated entries) so the jump stays inside this compiled body."""
    L = []
    if i.kind == D.BRANCH:
        # BRANCH-AND-LINK (bltzal / bgezal) is a CONDITIONAL CALL, not a conditional branch: the
        # subroutine returns via `jr $ra` and execution RESUMES at addr+8. It must therefore call and
        # then FALL THROUGH — never `goto`, never `return`.
        #
        # Both of the ordinary-branch paths below are wrong for it, and were being used:
        #   goto L_<target>  jumps INTO the subroutine's body, whose `jr $ra` the emitter renders as a
        #                    bare `return;` — so the ENCLOSING function exits early, skipping its
        #                    epilogue and leaking its stack frame. Every ancestor frame then shifts,
        #                    and an outer function reloads a callee-saved register from the wrong
        #                    stack word. Measured on Spider-Man: gen_func_8007C4D8 exited 40 bytes low
        #                    via L_8007D160, which is how $fp (a global base held at 0x800B0000) came
        #                    back as 0x00000000 and then as garbage in main().
        #   call + return    returns from the enclosing function instead of resuming at addr+8.
        #
        # 131 sites across this game's substrate were mis-emitted this way (115 goto, 16 call+return).
        # Discovery only followed `jal`, so link-branch targets were never function entries — see
        # discover_funcs.
        if i.op in ("bltzal", "bgezal"):
            # Order matters: MIPS evaluates the rs comparison BEFORE writing $ra, and writes $ra
            # UNCONDITIONALLY (taken or not). Writing the link first would corrupt the condition
            # whenever rs == 31. Then the delay slot runs, then the call.
            # One braced statement, like the ordinary-branch path below: a function-scope declaration
            # here would make every `goto` in the function jump across an initialisation, which this
            # dialect rejects outright.
            cond = BRANCH_COND[i.op](i)
            L.append(f"  {{ int _t = ({cond}); {R(31)} = 0x{i.addr + 8:08X}u; {ds_c} "
                     f"if (_t) {{ {call_or_dispatch(i.target, funcset, N)} }} }}")
            return L
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
        elif ra_tail:
            # TAIL-RETURN THROUGH A NON-`ra` REGISTER. The guest moved `ra` into another register and
            # jumps through it, so this is a RETURN, not a computed call — but it is indistinguishable
            # from one at the jump itself, and dispatching it fail-fasts: the target is the instruction
            # after some caller's `jal`, i.e. mid-function, which is never a function entry.
            # (Spyro: 0x80053594 `addi a3,ra,0` then 0x800535B8 `jr a3` lands on 0x80038620, exactly the
            # instruction after `jal 0x800530C0`.) Emitting `return` lets the C stack unwind to the real
            # caller, which resumes after its own call site — precisely the guest semantics.
            # Decided STATICALLY by ra_tail_returns(), so it cannot mask a genuine computed call: only
            # a jr whose register provably came from `ra` within the function takes this path.
            L.append(f"  {ds_c} return;   /* tail-return via {R(i.rs)} (= ra) */")
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


def overlay_funcs(exe, overlay_dir, base=None):
    """Seed resident functions that are reached ONLY from the stage overlays (\\BIN\\*.BIN, loaded
    raw to `base` at runtime). Those overlays `jal` into resident MAIN.EXE functions (the cooperative
    scheduler, file loaders, etc.) that direct-jal discovery within MAIN.EXE never sees. We scan each
    overlay's words for `jal` targets landing in MAIN.EXE text whose first word decodes as a real
    instruction (filters data/jump-table false positives), and seed them; discover_funcs then follows
    their call graph. Pure binary input (the overlays are game data, like MAIN.EXE) — no Ghidra.
    Skipped if absent.

    `base` is only the PC used to decode each word, and a `jal` target is
    `(PC & 0xF0000000) | (imm26 << 2)` — so only its top nibble can matter, and every overlay shares
    that nibble with the resident image. Default to the image's own load address rather than naming
    any particular game's overlay slot."""
    lo, hi = exe.load, exe.text_end
    if base is None:
        base = exe.load
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
                # `jal` is not the only CALL. bltzal/bgezal link too, so their targets are function
                # entries by definition. Following only `jal` left those targets as bare labels inside
                # whatever function happened to contain them, and emit_control then `goto`-ed into a
                # body whose `jr $ra` exits the ENCLOSING function — see the branch-and-link note in
                # emit_control for the stack-frame leak that causes.
                if ins.op in ("jal", "bltzal", "bgezal") and lo <= ins.target < hi \
                        and ins.target not in funcs:
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
        # DEFERRED WORK: a static recompile has no instruction boundary to interrupt at, so
        # deferred work is taken at guest FUNCTION entry — the one point that is guaranteed
        # call-coherent (o32 says caller-saved registers are dead here, and no native runtime routine
        # is midway through mutating hardware state). One predictable load-and-test per call; the
        # poll itself runs only when there is something to do.
        #
        # The gate word carries two independent kinds of work (Core::PW_IRQ / PW_HOST, core.h). The
        # second is what gives the HOST a turn while the guest runs straight-line code — without it a
        # guest busy-wait paced by a per-vblank callback can never terminate, because nothing
        # advances time between two guest instructions. Both share one word so the hot path cost is
        # unchanged: still a single load-and-test, however many kinds of work exist.
        d.append(f"void {N.wrap}_{a:08X}(Core* c) {{ c->pc = 0x{a:08X}u; "
                 f"if (c->pending_work) rec_irq_poll(c); if ({N.ovtab}[{i}]) {{ "
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
    # NATIVE-DEPTH COVERAGE, reported at BUILD time. At run time "0 vertices recorded" and "this game
    # has no 3D" are the same observation, so the number of taps the emitter actually placed has to be
    # visible here — a game whose submit idiom neither form matches shows up as 0 now, not as a mystery
    # later. `swc2` sites are inline (GTE_SCREEN_XY_REGS); `mfc2->sw` pairs come from vertex_pz_stores.
    global g_pz_tapped, g_pz_skipped
    print(f"[{N.wrap}] native-depth: {g_pz_tapped} mfc2->sw vertex store(s) tapped"
          + (f", {g_pz_skipped} skipped (store sits in a branch delay slot)" if g_pz_skipped else ""))
    g_pz_tapped = 0; g_pz_skipped = 0
    # The source TUs this module wrote (basenames) — collected into generated/rec_sources.cmake so
    # the build links exactly what was emitted (overlay count is dynamic).
    return [f"{N.disp}.c"] + [f"{N.shardpfx}_{s}.c" for s in range(shards)]


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: emit.py <MAIN.EXE> <out.c> [--seeds FILE] [--overlays DIR] "
                 "[--stub SCUS.EXE] [--limit N]")
    exe = psexe.load(sys.argv[1])
    out_path = sys.argv[2]
    limit = None
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])

    # Seed purely from the BINARY — the entry point + the pointer scans — plus the GAME's OWN seed
    # file (--seeds). Functions reached only indirectly (jalr through a function pointer), or entered
    # as a runtime-computed re-entry point, are invisible to direct-jal discovery, and WHICH ones
    # those are is a fact about the specific executable, not about the framework. Discovery then
    # grows the recompiled set by following direct jal targets (discover_funcs).
    #
    # The interpreter is GONE (later-254): a fn reached only via a function pointer that no scan sees
    # is NOT recompiled, so a call to it FAILS FAST at runtime — add it to the game's seed file when
    # the boot surfaces it as a [recomp-MISS], with a note on how it is reached. (Set
    # PSXPORT_USE_GHIDRA=1 to additionally seed from the Ghidra decomp, recompiling more up-front.)
    if "--seeds" in sys.argv:
        gs = load_seeds(sys.argv[sys.argv.index("--seeds") + 1])
    else:
        # Say so LOUDLY. A game that needs seeds but forgets the flag still emits a complete-looking
        # set — just a smaller one — and only fails much later as a runtime [recomp-MISS]. Announcing
        # it here is the difference between a one-line cause and a debugging session.
        print("[recomp] no --seeds file: discovery is BINARY-ONLY (entry point + pointer scans + jal "
              "graph). Any function reached only via a fn-pointer or a runtime-computed re-entry will "
              "be missing and fail fast at runtime as a [recomp-MISS].")
        gs = EMPTY_SEEDS
    check_seeds_in_text(exe, gs["main"] | gs["main_reentry"], "--seeds main/main_reentry")
    ov_dir = sys.argv[sys.argv.index("--overlays") + 1] if "--overlays" in sys.argv else None
    seeds = ({exe.entry} | gs["main"] | pointer_table_funcs(exe)
             | constructed_func_pointers(exe) | code_pointer_tables(exe)
             | overlay_data_func_pointers(exe, ov_dir))   # object-behavior handlers in overlay templates
    out_dir = os.path.dirname(out_path) or "."
    # Output is split into SHARDS translation units so the build compiles them in parallel.
    SHARDS = max(1, int(os.environ.get("PSXPORT_SHARDS", "8")))
    # NOTE: --overlays is NOT passed to the MAIN module's emit_module anymore (no `ov_dir`). The old
    # overlay_funcs() seeding (resident fns the overlays jal into) is still useful, so keep it:
    # Mid-function re-entry seeds — an entry inside another function whose body must fall THROUGH
    # into the seed (not `return`) — come from the game's seed file (`main_reentry`).
    src_files = emit_module(exe, out_dir, MAIN_NAMES, seeds, ov_dir, limit, SHARDS,
                            reentry=gs["main_reentry"])

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
    # the base). A base is therefore a fact about the SPECIFIC DISC, and the game supplies it in its
    # own seed file (`overlay_bases`, or `overlay_base_patterns` for a whole family that shares one
    # slot — e.g. interchangeable per-area code overlays). Capture the real load destinations with
    # PSXPORT_DEBUG=cd over a boot->field run; they are deterministic game facts, NOT magic offsets.
    # A base that is merely GUESSED is unrecoverable garbage rather than an error, so a missing one
    # fails fast below instead of defaulting.
    def overlay_base(stem):
        if stem in gs["overlay_bases"]:
            return gs["overlay_bases"][stem]
        for rx, base in gs["overlay_base_patterns"]:
            if re.fullmatch(rx, stem):
                return base
        return None
    # Explicit per-overlay seeds: addresses reached NOT by a jal/pointer the scans see, but as a CLEAN
    # RE-ENTRY POINT the runtime resumes at (a cooperative loop top, a stage entry the native path
    # owns, a hard-coded absolute call into another overlay's slot). Game facts — supplied by the
    # game's seed file under `overlay_seeds`.
    OVERLAY_EXTRA_SEEDS = gs["overlay_seeds"]
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
                # NEVER guess a base. An overlay is keyed BY its load address, so a wrong base emits a
                # whole module of correctly-decoded instructions at wrong addresses — every jal target,
                # every pointer-table test and the runtime router are then silently wrong. That is
                # unrecoverable-looking garbage rather than an error, so fail fast instead.
                sys.exit(f"[recomp] overlay {fn}: no load base in the game's seed file. Capture the real "
                         f"load destination (PSXPORT_DEBUG=cd over a boot->field run) and add it under "
                         f"`overlay_bases` (or `overlay_base_patterns` for a family sharing one slot).")
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
