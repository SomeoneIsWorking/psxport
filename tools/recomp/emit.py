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
    """Symbol naming for one emitted module. A second EXE (the boot stub) that overlaps MAIN.EXE's
    address space gets its OWN module — distinct wrapper/body/dispatch/override symbols + output
    files — so func_<addr> never collides between the two images. Both share rec_dispatch_miss
    (BIOS/interp) for dispatch misses."""
    def __init__(self, gen, wrap, dispatch, index, setov, ovtab, decls, shardpfx, disp):
        self.gen, self.wrap, self.dispatch = gen, wrap, dispatch
        self.index, self.setov, self.ovtab = index, setov, ovtab
        self.decls, self.shardpfx, self.disp = decls, shardpfx, disp

MAIN_NAMES = Names("gen_func", "func", "rec_dispatch", "rec_func_index", "shard_set_override",
                   "g_override", "rec_decls.h", "shard", "shard_disp")
STUB_NAMES = Names("stub_gen_func", "stub_func", "stub_dispatch", "stub_func_index",
                   "stub_set_override", "g_stub_override", "stub_decls.h", "stub_shard", "stub_disp")


def find_jump_tables(exe, ins, lo, hi, validate=True):
    """Recover in-function jump tables (C `switch`) so a computed `jr` stays INSIDE the compiled body
    instead of routing through rec_dispatch (which, under the no-interpreter substrate, would dispatch
    the table's mid-function case labels as fake functions -> stack corruption; see docs/native-port-
    plan.md). Detects the MIPS switch idiom around a `jr rN` (rN != ra):
        sltiu cond, idx, COUNT ; (beqz cond, default) ; sll t, idx, 2
        lui   base, HI ; [addiu base, base, LO] ; addu base, base, t ; lw rN, OFF(base) ; jr rN
    The jump table is at HI<<16 (+LO) + OFF; read COUNT word targets from the EXE image. Returns
    {jr_addr: [target_addr,...]} (targets are the case-label code addresses)."""
    jt = {}
    for a in sorted(ins):
        i = ins[a]
        if not (i.kind == D.JUMPR and i.op == "jr" and i.rs and i.rs != 31):
            continue
        base_reg = off = hi_val = lo_add = count = None
        for b in range(a - 4, max(lo, a - 0x40) - 4, -4):
            if b not in ins:
                break
            j = ins[b]
            if base_reg is None and j.op == "lw" and j.rt == i.rs:   # the table load into the jr reg
                base_reg, off = j.rs, j.simm
                continue
            if base_reg is not None:
                if hi_val is None and j.op == "lui" and j.rt == base_reg:
                    hi_val = j.imm << 16
                elif lo_add is None and j.op == "addiu" and j.rt == base_reg and j.rs == base_reg:
                    lo_add = j.simm
            if count is None and j.op == "sltiu":
                count = j.imm
            if hi_val is not None and count is not None:
                break
        if base_reg is None or hi_val is None or off is None or not count or count > 4096:
            continue
        tbl = (hi_val + (lo_add or 0) + off) & 0xFFFFFFFF
        try:
            targets = [exe.word(tbl + k * 4) for k in range(count)]
        except Exception:
            continue
        if validate and any(not (lo <= t < hi) for t in targets):  # a real switch jumps within its fn
            continue
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


def emit_func(exe, lo, hi, funcset, out, name, N):
    """Emit one C function covering [lo, hi) under the given C name (the pure recomp body)."""
    ins = {a: decode(a, exe.word(a)) for a in range(lo, hi, 4)}
    jt = find_jump_tables(exe, ins, lo, hi)

    # Simulate the emission walk to find the exact set of "standalone" addresses (those
    # emitted as their own statement; a control op consumes the next word as its delay
    # slot). A branch/jump target is a real label only if it coincides with a standalone
    # address — targets that land inside data (or on a consumed delay slot) instead route
    # through rec_dispatch, so generated code never references an undefined label.
    standalone, a = set(), lo
    while a < hi:
        standalone.add(a)
        a += 8 if ins[a].kind in (D.BRANCH, D.JUMP, D.JUMPR) else 4
    labels = {i.target for i in ins.values()
              if i.kind in (D.BRANCH, D.JUMP) and i.target in standalone and lo <= i.target < hi}
    # jump-table case-label targets are real labels too (the recovered `switch` gotos into them)
    for tgts in jt.values():
        for t in tgts:
            if t in standalone:
                labels.add(t)

    out.append(f"void {name}(Core* c) {{")
    a = lo
    while a < hi:
        i = ins[a]
        if a in labels:
            out.append(f"L_{a:08X}:;")
        if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
            slot = ins.get(a + 4)
            ds_c = emit_simple(slot) if (slot and slot.kind not in
                   (D.BRANCH, D.JUMP, D.JUMPR)) else "/* DS */"
            out.extend(emit_control(i, ds_c, funcset, labels, N, jt.get(a)))
            a += 8
        else:
            s = emit_simple(i)
            if s:
                out.append("  " + s)
            a += 4
    out.append("  return;")
    out.append("}")
    out.append("")


def call_or_dispatch(target, funcset, N):
    return (f"{N.wrap}_{target:08X}(c);" if target in funcset
            else f"{N.dispatch}(c, 0x{target:08X}u);")


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
                     f"default: {N.dispatch}(c, {R(i.rs)}); return; }} }}")
        else:
            L.append(f"  {ds_c} {N.dispatch}(c, {R(i.rs)}); return;")
    else:  # jalr rd, rs
        L.append(f"  {R(i.rd)} = 0x{i.addr + 8:08X}u;")
        L.append(f"  {ds_c} {N.dispatch}(c, {R(i.rs)});")
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


def pointer_table_funcs(exe):
    """Seed functions reached ONLY via a function pointer (jalr through a table / vtable slot) —
    invisible to direct-jal discovery. With the interpreter gone (later-254) a call to such a fn fails
    fast, so we must recompile them. Scan the WHOLE EXE image (text + data) for words that point at a
    function ENTRY in text (is_func_entry). discover_funcs then follows each one's direct-jal call
    graph. (A truly stackless leaf that is also NOT preceded by `jr ra` — e.g. the first fn after a
    data island — still needs a manual EXTRA_SEEDS when the boot surfaces it.)"""
    lo, hi = exe.load, exe.text_end
    return {exe.word(a) for a in range(lo, hi, 4) if is_func_entry(exe, exe.word(a))}


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
        for b in range(a + 4, a + 24, 4):
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


def emit_module(exe, out_dir, N, seeds, ov_dir=None, limit=None, shards=8):
    """Discover the recompiled function set for `exe` and emit its module (shards + dispatch TU +
    decls header) under the symbol/file names in `N`. Shared by the MAIN.EXE module and the boot-
    stub module; the stub gets distinct names so its func_<addr> don't collide with MAIN's."""
    ov = overlay_funcs(exe, ov_dir)
    if ov:
        print(f"[{N.wrap}] overlay seeds: {len(ov)} resident fns reached from {ov_dir}")
    seeds = set(seeds) | ov
    if os.environ.get("PSXPORT_USE_GHIDRA"):
        seeds |= set(ghidra_funcs(exe.load, exe.text_end))
    funcs = discover_funcs(exe, seeds)
    # PRUNE jump-table case labels wrongly seeded/discovered as functions: they are mid-function code,
    # and leaving them in truncates the containing function so its switch can't be recovered (the
    # substrate-derail root cause). Keep any that ARE a seed entry (defensive: a real fn shouldn't be a
    # case label, but never drop an explicit seed).
    jt_labels = collect_jt_targets(exe, funcs, exe.text_end) - set(seeds)
    if jt_labels:
        funcs = [f for f in funcs if f not in jt_labels]
        print(f"[{N.wrap}] pruned {len(jt_labels)} jump-table case labels from the function set")
    print(f"[{N.wrap}] functions: {len(seeds)} seeds -> {len(funcs)} recompiled after jal "
          f"discovery (rest run via the interpreter)")
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
        emit_func(exe, a, nxt_of[a], funcset, shard[k % shards], f"{N.gen}_{a:08X}", N)
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
        # --- engine render functions reached ONLY via a function pointer (so direct-jal discovery
        #     misses them) — seeded to EMIT readable C for the native-engine RE/port (later-135). The
        #     runtime is interpreter-only, so seeding doesn't change execution; it makes the body
        #     available in generated/ for RE (and rec_set_override works on them by address regardless).
        0x8002AB5C,  # field terrain/map renderer — node+24 render-fn ptr of the t32 render-list node
        0x80051C8C,  # per-object transform builder (node+0x98 matrix from euler angles + position)
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
    seeds = {exe.entry} | EXTRA_SEEDS | pointer_table_funcs(exe) | constructed_func_pointers(exe)
    ov_dir = sys.argv[sys.argv.index("--overlays") + 1] if "--overlays" in sys.argv else None
    out_dir = os.path.dirname(out_path) or "."
    # Output is split into SHARDS translation units so the build compiles them in parallel.
    SHARDS = max(1, int(os.environ.get("PSXPORT_SHARDS", "8")))
    emit_module(exe, out_dir, MAIN_NAMES, seeds, ov_dir, limit, SHARDS)

    # The disc's boot stub (SCUS_944.54): the real PSX entry — draws SCEA, then LoadExec's MAIN.
    # It overlaps MAIN.EXE's address space, so it is emitted as a SEPARATE module (STUB_NAMES) with
    # its own dispatch/override symbols (stub_dispatch/stub_set_override) — see native_stub.c. Its
    # only seed is its entry; discovery follows the stub's own jal graph.
    if "--stub" in sys.argv:
        stub = psexe.load(sys.argv[sys.argv.index("--stub") + 1])
        emit_module(stub, out_dir, STUB_NAMES, {stub.entry}, None, None, shards=2)

    # Stub the old monolith path so a stale copy is never compiled.
    open(out_path, "w").write("// recompiled core is split into shard_*.c — see rec_decls.h\n")


if __name__ == "__main__":
    main()
