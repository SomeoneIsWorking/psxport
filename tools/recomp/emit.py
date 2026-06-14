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
            return wr(ins.rt, f"(uint32_t)(int8_t)mem_r8({a})")
        if o == "lbu":
            return wr(ins.rt, f"(uint32_t)mem_r8({a})")
        if o == "lh":
            return wr(ins.rt, f"(uint32_t)(int16_t)mem_r16({a})")
        if o == "lhu":
            return wr(ins.rt, f"(uint32_t)mem_r16({a})")
        if o == "lw":
            return wr(ins.rt, f"mem_r32({a})")
        if o == "lwl":
            return wr(ins.rt, f"mem_lwl({R(ins.rt)}, {a})")
        if o == "lwr":
            return wr(ins.rt, f"mem_lwr({R(ins.rt)}, {a})")
    if k == D.STORE:
        a = addr_expr(ins)
        if o == "sb":
            return f"mem_w8({a}, (uint8_t){R(ins.rt)});"
        if o == "sh":
            return f"mem_w16({a}, (uint16_t){R(ins.rt)});"
        if o == "sw":
            return f"mem_w32({a}, {R(ins.rt)});"
        if o == "swl":
            return f"mem_swl({a}, {R(ins.rt)});"
        if o == "swr":
            return f"mem_swr({a}, {R(ins.rt)});"
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
        return f"gte_write_data({ins.rt}, mem_r32({addr_expr(ins)}));"
    if k == D.GTE_STORE:  # swc2: mem[rs+imm] = cop2_data[rt]
        return f"mem_w32({addr_expr(ins)}, gte_read_data({ins.rt}));"
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


def emit_func(exe, lo, hi, funcset, out, name):
    """Emit one C function covering [lo, hi) under the given C name (the pure recomp body)."""
    ins = {a: decode(a, exe.word(a)) for a in range(lo, hi, 4)}

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

    out.append(f"void {name}(R3000* c) {{")
    a = lo
    while a < hi:
        i = ins[a]
        if a in labels:
            out.append(f"L_{a:08X}:;")
        if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
            slot = ins.get(a + 4)
            ds_c = emit_simple(slot) if (slot and slot.kind not in
                   (D.BRANCH, D.JUMP, D.JUMPR)) else "/* DS */"
            out.extend(emit_control(i, ds_c, funcset, labels))
            a += 8
        else:
            s = emit_simple(i)
            if s:
                out.append("  " + s)
            a += 4
    out.append("  return;")
    out.append("}")
    out.append("")


def call_or_dispatch(target, funcset):
    return f"func_{target:08X}(c);" if target in funcset else f"rec_dispatch(c, 0x{target:08X}u);"


def emit_control(i, ds_c, funcset, labels):
    """Lines for a control instruction `i` whose delay-slot C is `ds_c`."""
    L = []
    if i.kind == D.BRANCH:
        if i.op in ("bltzal", "bgezal"):
            L.append(f"  {R(31)} = 0x{i.addr + 8:08X}u;")
        cond = BRANCH_COND[i.op](i)
        if i.target in labels:
            tgt = f"goto L_{i.target:08X};"
        else:
            tgt = "{ " + call_or_dispatch(i.target, funcset) + " return; }"
        L.append(f"  {{ int _t = ({cond}); {ds_c} if (_t) {tgt} }}")
        return L
    if i.kind == D.JUMP:
        if i.op == "jal":
            L.append(f"  {R(31)} = 0x{i.addr + 8:08X}u;")
            L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset)}")
        else:  # j
            if i.target in labels:
                L.append(f"  {ds_c} goto L_{i.target:08X};")
            else:
                L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset)} return;")
        return L
    # JUMPR
    if i.op == "jr":
        if i.rs == 31:
            L.append(f"  {ds_c} return;")
        else:
            L.append(f"  {ds_c} rec_dispatch(c, {R(i.rs)}); return;")
    else:  # jalr rd, rs
        L.append(f"  {R(i.rd)} = 0x{i.addr + 8:08X}u;")
        L.append(f"  {ds_c} rec_dispatch(c, {R(i.rs)});")
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


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: emit.py <MAIN.EXE> <out.c> [--limit N]")
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
        # --- native-override targets: seed so the func_<addr> wrapper exists and
        #     rec_set_override(addr,fn) reaches them (rec_set_override only works on RECOMPILED
        #     entries). libcard B0-vector I/O trampolines, overridden by memcard.c for native
        #     synchronous card file I/O (see runtime/recomp/memcard.c). All three are already
        #     reachable via direct jal (so this is a harmless no-op today), seeded proactively
        #     so the override contract never depends on jal-discovery happening to reach them.
        0x8009BAF0,  # _card_read   (B0:0x4E) — override target (memcard.c ov_card_read)
        0x8009C600,  # _card_write  (B0:0x4F) — override target (memcard.c ov_card_write)
        0x8009C610,  # _card_status (B0:0x5C) — override target (memcard.c ov_card_status)
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
    # function list: anything reached only via a function pointer (jalr) that discovery can't see
    # is run by the hybrid interpreter (interp.c) at runtime, faithfully. (Set PSXPORT_USE_GHIDRA=1
    # to additionally seed from the Ghidra decomp / committed list, recompiling more for speed.)
    seeds = {exe.entry} | EXTRA_SEEDS
    # Resident functions reached only from the stage overlays (binary-derived seeds).
    ov_dir = sys.argv[sys.argv.index("--overlays") + 1] if "--overlays" in sys.argv else None
    ov = overlay_funcs(exe, ov_dir)
    if ov:
        print(f"overlay seeds: {len(ov)} resident fns reached from {ov_dir}")
    seeds |= ov
    if os.environ.get("PSXPORT_USE_GHIDRA"):
        seeds |= set(ghidra_funcs(exe.load, exe.text_end))
    funcs = discover_funcs(exe, seeds)
    print(f"functions: {len(seeds)} seeds -> {len(funcs)} recompiled after jal discovery "
          f"(rest run via the interpreter)")
    funcset = set(funcs)
    if limit:
        funcs = funcs[:limit]

    ordered = sorted(funcset)
    nxt_of = {f: (ordered[k + 1] if k + 1 < len(ordered) else exe.text_end)
              for k, f in enumerate(ordered)}
    idx = {a: k for k, a in enumerate(funcs)}
    out_dir = os.path.dirname(out_path) or "."
    # Output is split into SHARDS translation units so the build can compile them in parallel
    # (the recompiled core is large; one TU was the build bottleneck). Layout:
    #   rec_decls.h   — forward decls of every gen_func_X / func_X + the override-table extern
    #   shard_<n>.c   — gen_func_X bodies, round-robin across shards
    #   shard_disp.c  — override table, func_X wrappers, rec_func_index/set_override/dispatch
    SHARDS = max(1, int(os.environ.get("PSXPORT_SHARDS", "8")))

    hdr = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", "#pragma once",
           '#include "r3000.h"', ""]
    # Per function a pure recomp body gen_func_X and a wrapper func_X (the wrapper checks a
    # runtime override slot then calls the body — body stays alive for A/B + diffing).
    for a in funcs:
        hdr.append(f"void gen_func_{a:08X}(R3000*); void func_{a:08X}(R3000*);")
    hdr.append("extern OverrideFn g_override[];")
    open(os.path.join(out_dir, "rec_decls.h"), "w").write("\n".join(hdr) + "\n")

    shard = [["// GENERATED — DO NOT EDIT.", '#include "rec_decls.h"', ""] for _ in range(SHARDS)]
    for k, a in enumerate(funcs):
        emit_func(exe, a, nxt_of[a], funcset, shard[k % SHARDS], name=f"gen_func_{a:08X}")
    for s in range(SHARDS):
        open(os.path.join(out_dir, f"shard_{s}.c"), "w").write("\n".join(shard[s]) + "\n")

    # Dispatch TU: override table + wrappers + the address->fn switches. Overrides super-call
    # gen_func_X directly to run the original body without re-entering the wrapper.
    d = ["// GENERATED — DO NOT EDIT.", '#include "rec_decls.h"', "",
         f"OverrideFn g_override[{len(funcs)}];"]
    for a in funcs:
        i = idx[a]
        d.append(f"void func_{a:08X}(R3000* c) {{ if (g_override[{i}]) {{ g_override[{i}](c); return; }} gen_func_{a:08X}(c); }}")
    d.append("int rec_func_index(uint32_t addr) {\n  switch (addr & 0x1FFFFFFFu) {")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: return {idx[a]};")
    d.append("    default: return -1;\n  }\n}")
    d.append("void rec_set_override(uint32_t addr, OverrideFn fn) "
             "{ int i = rec_func_index(addr); if (i >= 0) g_override[i] = fn; }")
    d.append("void rec_dispatch(R3000* c, uint32_t addr) {\n  switch (addr & 0x1FFFFFFFu) {")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: func_{a:08X}(c); return;")
    d.append("    default: rec_dispatch_miss(c, addr); return;\n  }\n}")
    open(os.path.join(out_dir, "shard_disp.c"), "w").write("\n".join(d) + "\n")

    # Stub the old monolith path so a stale copy is never compiled.
    open(out_path, "w").write("// recompiled core is split into shard_*.c — see rec_decls.h\n")
    print(f"emitted {len(funcs)} functions -> {out_dir}/shard_*.c ({SHARDS} shards) + rec_decls.h")


if __name__ == "__main__":
    main()
