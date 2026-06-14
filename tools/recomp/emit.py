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


def emit_func(exe, lo, hi, funcset, out):
    """Emit one C function covering [lo, hi)."""
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

    out.append(f"void func_{lo:08X}(R3000* c) {{")
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
    return sorted({int(x, 16) for x in re.findall(
        r'==================== ([0-9A-F]{8}) FUN_', open(decomp).read())
        if text_lo <= int(x, 16) < text_hi})


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

    seeds = set(ghidra_funcs(exe.load, exe.text_end)) | {exe.entry}
    funcs = discover_funcs(exe, seeds)
    print(f"functions: {len(seeds)} seeds (Ghidra+entry) -> {len(funcs)} after jal discovery")
    funcset = set(funcs)
    if limit:
        funcs = funcs[:limit]

    out = [
        "// GENERATED by tools/recomp/emit.py — DO NOT EDIT. Regenerate from MAIN.EXE.",
        '#include "r3000.h"', "",
    ]
    # Forward declarations.
    for a in funcset:
        out.append(f"void func_{a:08X}(R3000*);")
    out.append("")

    ordered = sorted(funcset)
    nxt_of = {f: (ordered[k + 1] if k + 1 < len(ordered) else exe.text_end)
              for k, f in enumerate(ordered)}
    for a in funcs:
        emit_func(exe, a, nxt_of[a], funcset, out)

    # Dispatch table (generated): address -> recompiled function. Misses (BIOS vectors,
    # overlay code, computed targets) route to the hand-written runtime hook.
    out.append("void rec_dispatch(R3000* c, uint32_t addr) {")
    out.append("  switch (addr & 0x1FFFFFFFu) {")
    for a in funcs:
        out.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: func_{a:08X}(c); return;")
    out.append("    default: rec_dispatch_miss(c, addr); return;")
    out.append("  }")
    out.append("}")

    open(out_path, "w").write("\n".join(out) + "\n")
    print(f"emitted {len(funcs)} functions -> {out_path} ({os.path.getsize(out_path)} bytes)")


if __name__ == "__main__":
    main()
