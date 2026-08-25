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

    def checkpoints(vs):
        result = set()
        for i, value in enumerate(vs):
            where = f"diagnostic_pcs[{i}]"
            if isinstance(value, dict):
                if set(value) != {"owner", "pc"}:
                    sys.exit(f"[recomp] {path}: {where}: expected exactly owner and pc")
                result.add((addr(value["owner"], where + ".owner"),
                            addr(value["pc"], where + ".pc")))
            else:
                result.add((None, addr(value, where)))
        return result

    known = {"main", "main_reentry", "overlay_bases", "overlay_base_patterns", "overlay_seeds",
             "diagnostic_pcs"}
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
        "diagnostic_pcs": checkpoints(data.get("diagnostic_pcs", [])),
    }


EMPTY_SEEDS = {"main": set(), "main_reentry": set(), "overlay_bases": {},
               "overlay_base_patterns": [], "overlay_seeds": {}, "diagnostic_pcs": set()}


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

RECOMP_VERSION = "2026-08-25.2"   # cross-overlay call seeds; delay slots are never dropped

R = lambda n: f"c->r[{n}]"


def wr(n, expr):
    """Assignment to GPR n, discarding writes to r0."""
    return "" if n == 0 else f"{R(n)} = {expr};"


# ==================================================================================================
# BASE-RELATIVE (relocatable) module emission
#
# A game whose code modules are loaded and RELOCATED at runtime cannot have them pinned to one guest
# address: two modules resident at once would occupy the same bytes. Such a module is instead emitted
# BASE-RELATIVE — recompiled from an image relocated at an arbitrary LINK base B0, with a per-Core
# `delta` (= live base − B0) added at exactly the sites whose value moves with the module.
#
# WHICH SITES, AND WHY THAT SET IS COMPLETE. A recompiled body holds a guest address in only four
# shapes, and each has one rule:
#
#   1. an address COMPOSED at runtime from a `lui` + a 16-bit low half. The game's relocation table
#      names the `lui` (its HI16 entries). Adding delta to what the `lui` produces gives
#      a(B0) + delta = a(B) for EVERY low-half partner of that `lui`, however many there are and
#      whatever their carry — because the emitter never re-splits hi/lo, delta simply passes through
#      the addition. So the low halves are emitted completely UNCHANGED. (This is the whole reason
#      the model is cheap: 911 `lui` sites carry the 978 low halves for free.)
#   2. an address the emitted code hands to the ROUTER (`rec_dispatch`) — a `j`/`jal`/branch target
#      outside this module's own function set. The criterion is the ADDRESS RANGE, not the
#      relocation type: branch targets are PC-relative so the relocation table never names them, yet
#      they move with the module exactly the same. A constant inside [B0, B0+size) gets delta; one
#      outside (the resident executable, a BIOS vector) must not.
#   3. a LINK register value (`jal`/`jalr`/`bltzal`/`bgezal` write `addr + 8`), always in-module.
#   4. a runtime address the body switches on (a recovered jump table's target register). The table
#      words are relocated by the GUEST, so the register holds a live address and the emitted `case`
#      labels are link-space — subtract delta before matching.
#
# Everything else is either a compile-time C label (`goto`), a direct call to a C symbol, or data
# read out of guest RAM through `c->mem_r*` (which the guest's own relocator has already fixed up).
#
# The MODULE'S OWN address->function switch takes a RUNTIME address (that is what the router hands
# it, and what a dispatch miss must report) and subtracts delta itself. `<mod>_func_index` and
# `<mod>_set_override` take a LINK-SPACE address instead: they are build-time keys — you register an
# override against the address you read in a disassembly — and they have no Core to read delta from.
class ModuleReloc:
    """Base-relative emission context for ONE relocatable module. `hi16` is the set of guest
    addresses (in LINK space) at which the game's relocation table names a HI16 site."""

    def __init__(self, index, link_lo, link_hi, hi16):
        self.index = index                       # slot in Core::ovDelta[] — the emit-time overlay ordinal
        self.link_lo, self.link_hi = link_lo, link_hi
        self.hi16 = hi16
        self.delta = f"c->ovDelta[{index}]"

    def in_module(self, a):
        return self.link_lo <= a < self.link_hi


g_mod = None      # the ModuleReloc being emitted, or None for an ordinary (fixed-base) module

# Core::ovDelta[] capacity (runtime/recomp/core.h, kRecMaxOverlays). A relocatable module's delta is
# reached from generated code as `c->ovDelta[<ordinal>]`, so the table has to be a fixed-size member.
# Exceeding it is a build ERROR, never a truncation.
REC_MAX_OVERLAYS = 64


def load_reloc_sidecar(path, base, size, fn):
    """`<stem>.reloc.json` -> the set of LINK-space addresses at which the game relocates a HI16, or
    None when the module has no sidecar (an ordinary fixed-base overlay).

    THE SIDECAR IS A GAME ARTIFACT, and deliberately dumb: the game's own extractor knows its
    console's relocation format, and hands the recompiler nothing but offsets. The framework does not
    parse a `.rel`, does not know a relocation encoding, and cannot acquire a game-specific one here.

        {"size": <module bytes>, "hi16": [offset, ...], "counts": {...}}

    `hi16` is the only field the emitter consumes; `counts` is carried so a mismatch between what the
    extractor saw and what the emitter got is visible rather than silent."""
    if not os.path.exists(path):
        return None
    with open(path) as f:
        side = json.load(f)
    if side.get("size") != size:
        sys.exit(f"[recomp] {fn}: relocation sidecar says the module is {side.get('size')} bytes but "
                 f"the image is {size}. The sidecar and the image must come from the same extraction "
                 f"— re-run the game's module extractor.")
    hi16 = {base + off for off in side["hi16"]}
    print(f"[recomp] {fn}: base-relative ({len(hi16)} HI16 site(s) from {os.path.basename(path)})")
    return hi16


# Instructions that may consume a `lui`-built address high half. The emitted `lui` result carries
# delta, so a consumer is SAFE exactly when delta passes through it into the composed address —
# i.e. when the register is used additively (a low-half add, a load/store base, an index add) or
# simply copied. Anything else (storing the raw high half, comparing it, masking or shifting it)
# would observe a value the console never held, because on hardware the relocated `lui` immediate is
# `hi(B)` while this emits `hi(B0) + delta`, which differ below bit 16 whenever delta is not
# 64K-aligned. Compiled code does not do that with a relocated `lui` — but "does not" is an
# assumption, so it is CHECKED, and a violation stops the build instead of shipping a wrong value.
_HI16_ADDITIVE_IMM = ("addiu", "addi")
_HI16_ADDITIVE_RRR = ("addu", "add", "subu", "sub")


# `rt` is a SOURCE in some encodings and a DESTINATION in others, and conflating the two makes a
# dataflow walk report a redefinition as a use. These two say which is which, per instruction kind.
_BRANCH_TWO_OPERAND = ("beq", "bne")


def reg_reads(i):
    k = i.kind
    if k == D.ALU_RRR or k == D.MULDIV or k == D.SHIFT_V:
        return {i.rs, i.rt}
    if k == D.ALU_RRI or k == D.LOAD or k == D.GTE_LOAD:
        return {i.rs}
    if k == D.STORE or k == D.GTE_STORE:
        return {i.rs, i.rt}
    if k == D.SHIFT_I:
        return {i.rt}
    if k == D.BRANCH:
        return {i.rs, i.rt} if i.op in _BRANCH_TWO_OPERAND else {i.rs}
    if k == D.JUMPR:
        return {i.rs}
    if k == D.GTE_MOVE:
        return {i.rt} if i.op in ("mtc2", "ctc2") else set()
    if k == D.COP0:
        return {i.rt} if i.op == "mtc0" else set()
    if k == D.HILO:
        return {i.rs} if i.op in ("mthi", "mtlo") else set()
    return set()


def reg_writes(i):
    k = i.kind
    if k in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V):
        return {i.rd}
    if k in (D.ALU_RRI, D.LUI, D.LOAD):
        return {i.rt}
    if k == D.GTE_MOVE:
        return {i.rt} if i.op in ("mfc2", "cfc2") else set()
    if k == D.COP0:
        return {i.rt} if i.op == "mfc0" else set()
    if k == D.HILO:
        return {i.rd} if i.op in ("mfhi", "mflo") else set()
    if k == D.JUMPR and i.op == "jalr":
        return {i.rd}
    return set()


def check_hi16_consumers(exe, hi16, fn):
    """Fail the emit if any HI16-relocated `lui` result is consumed in a way that would expose the
    high half itself rather than a composed address. Reports its denominator either way."""
    ins = {a: decode(a, exe.word(a)) for a in range(exe.load, exe.text_end, 4)}
    bad, uses = [], 0
    for site in sorted(hi16):
        i0 = ins.get(site)
        if i0 is None or i0.kind != D.LUI:
            bad.append((site, "the relocation table names a HI16 here, but the word is not a `lui`"))
            continue
        # `raw` = registers holding the HIGH HALF ALONE. That is the only value whose emitted form
        # (hi(B0) + delta) differs from the console's (hi(B)). The moment a low half is added the
        # register holds a COMPOSED address, which is bit-exact — so it leaves the set, and storing
        # or comparing it from then on is ordinary correct code.
        raw = {i0.rt}
        a = site + 4
        while a < exe.text_end and raw:
            i = ins.get(a)
            if i is None:
                break
            rd_regs, wr_regs = reg_reads(i), reg_writes(i)
            if raw & rd_regs:
                uses += 1
                if i.kind == D.ALU_RRI and i.op in _HI16_ADDITIVE_IMM and i.rs in raw:
                    raw.discard(i.rt)             # `addiu rt, raw, LO` — rt is now a full address
                elif i.kind in (D.LOAD, D.STORE, D.GTE_LOAD, D.GTE_STORE) and i.rs in raw \
                        and not (i.kind in (D.STORE, D.GTE_STORE) and i.rt in raw):
                    pass                          # `lw/sw x, LO(raw)` — composed in the addressing mode
                elif i.kind == D.ALU_RRR and i.op in _HI16_ADDITIVE_RRR and (i.rs in raw or i.rt in raw):
                    # `addu rd, raw, rX`. A register-register move (rX == $zero) COPIES the raw high
                    # half; anything else adds a real value, so rd is a composed address.
                    if i.rt == 0 or i.rs == 0:
                        raw.add(i.rd)
                    else:
                        raw.discard(i.rd)
                else:
                    bad.append((a, f"`{i.op}` consumes the RAW high half built at 0x{site:08X} "
                                   f"(rs=r{i.rs} rt=r{i.rt})"))
                    break
            # the raw value dies where its register is written by anything other than the `move` that
            # propagates it (which the read handler above has already re-added).
            for w in wr_regs:
                if w and not (i.kind == D.ALU_RRR and w == i.rd and (i.rt == 0 or i.rs == 0)
                              and (i.rs in raw or i.rt in raw)):
                    raw.discard(w)
            if i.kind in (D.JUMP, D.JUMPR):
                break                             # caller-saved liveness ends at a control transfer
            a += 4
    print(f"[recomp] {fn}: HI16 consumer check — {len(hi16)} site(s), {uses} use(s) examined, "
          f"{len(bad)} outside the additive class")
    if bad:
        for a, why in bad[:20]:
            print(f"[recomp]     0x{a:08X}: {why}")
        sys.exit(f"[recomp] {fn}: a HI16-relocated `lui` result escapes as a raw value. Base-relative "
                 f"emission adds the module delta to that register, so the escaping value would differ "
                 f"from the console's. This needs the emitter to model that site explicitly — it must "
                 f"not be waved through.")


def addr_const(a):
    """A guest-address CONSTANT the emitted body hands to the router or writes to a link register.
    Moves with the module when it names an in-module address (rule 2/3 above)."""
    if g_mod is not None and g_mod.in_module(a):
        return f"(0x{a:08X}u + (uint32_t)c->ovDelta[{g_mod.index}])"
    return f"0x{a:08X}u"


def link_space(expr):
    """A RUNTIME guest address computed by the body, expressed in LINK space so it can be matched
    against the emitter's compile-time `case` labels (rule 4 above)."""
    return f"({expr} - (uint32_t)c->ovDelta[{g_mod.index}])" if g_mod is not None else expr


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
        # A HI16 relocation site in a relocatable module: this `lui` builds the high half of an
        # in-module address, so its result carries delta for every low half that follows it.
        if g_mod is not None and ins.addr in g_mod.hi16:
            return wr(ins.rt, f"((uint32_t){ins.imm}u << 16) + (uint32_t)c->ovDelta[{g_mod.index}]")
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
        # Exact guest VA is diagnostic evidence: Core::pc is exact in the interpreter, but a static
        # recomp function may retain only its entry/block PC. The normal unarmed path still performs
        # the same GTE operation; the per-Core observer is a single null callback check.
        return f"gte_op_at(c, 0x{ins.cop2:08X}u, 0x{ins.addr:08X}u);"
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
        return f"rec_syscall(c, {(ins.raw >> 6) & 0xFFFFF}u, 0x{ins.addr:08X}u);"
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
        if any(t & 3 for t in targets):
            return None                 # R3000A instruction targets are always word-aligned
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


def _fold_immediate_constant(i, known):
    """Return the constant written by a standard MIPS immediate-construction instruction."""
    if i.op == "lui":
        return i.imm << 16
    if i.rs not in known:
        return None
    if i.op in ("addi", "addiu"):
        return (known[i.rs] + i.simm) & 0xFFFFFFFF
    if i.op == "ori":
        return known[i.rs] | i.imm
    return None


def _definite_gpr_write(i):
    """Return the GPR written by a decoded instruction, None for no GPR write, or -1 if unknown.

    This is intentionally separate from `defines_reg`: that helper is a depth-attribution safety
    predicate which treats every control instruction as a possible write to every queried register.
    Reaching-constant analysis needs the architectural answer instead. In particular, an ordinary
    branch does not clobber the register holding a computed-jump base; using `defines_reg` here made
    any branch between `lui`/`ori` and `jr` erase every known constant.
    """
    if i.kind in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V):
        return i.rd
    if i.kind in (D.ALU_RRI, D.LUI, D.LOAD):
        return i.rt
    if i.kind == D.HILO:
        return i.rd if i.op in ("mfhi", "mflo") else None
    if i.kind == D.GTE_MOVE:
        return i.rt if i.op in ("mfc2", "cfc2") else None
    if i.kind == D.COP0:
        return i.rt if i.op == "mfc0" else None
    if i.kind == D.BRANCH:
        return 31 if i.op in ("bltzal", "bgezal") else None
    if i.kind == D.JUMP:
        return 31 if i.op == "jal" else None
    if i.kind == D.JUMPR:
        return i.rd if i.op == "jalr" else None
    if i.kind in (D.STORE, D.GTE_STORE, D.GTE_LOAD, D.GTE_OP, D.MULDIV, D.NOP):
        return None
    return -1


def _advance_immediate_constants(i, known):
    """Advance a conservative reaching-constant map through one instruction."""
    value = _fold_immediate_constant(i, known)
    written = _definite_gpr_write(i)
    if written == -1:
        known.clear()
    elif written in known:
        del known[written]
    if value is not None and i.rt:
        known[i.rt] = value
    return value


def _scan_computed_offset(ins, jr_a, dst, lo, hi, window=160):
    """Recover `jr base + idx*2^k` — a computed OFFSET dispatch with NO address table.

    Shape:  lui + addiu/ori rB,<immediate base> ; sll rI,rI,k ; add dst,rB,rI ; jr dst

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
            if i.op in ("lui", "addi", "addiu", "ori"):
                break
            return None
    if add_a is None:
        # NO INDEX AT ALL: `lui rX,HI ; addiu/ori rX,rX,LO ; ... ; jr rX` — an indirect jump whose register
        # holds a plain IMMEDIATE, set at one or more sites in the function (Spyro's 0x800243FC reaches
        # 0x800240F8 or 0x800241A8 this way). There is no base+index to decompose, so the earlier scan
        # bails; the targets are simply the constants assigned to that register. Same principle as the
        # constant-index route — read the values the code actually puts there — applied one level up.
        consts, hi_v = set(), {}
        for a in range(lo, hi, 4):
            i = ins.get(a)
            if i is None:
                continue
            v = _advance_immediate_constants(i, hi_v)
            # This heuristic historically required a completed low-half assignment. A bare lui can
            # itself be a full constant when the low half is zero, but adding support for that is a
            # separate control-flow proof; silently counting it here turned one stale candidate into
            # a two-target false positive while adding ori support.
            if v is not None and i.op != "lui" and i.rt == dst and not (v & 3) and lo <= v < hi:
                consts.add(v)
        return sorted(consts) if len(consts) >= 2 else None

    # 2. the immediate base. FORWARD, because `lui rB,HI ; addiu/ori rB,rB,LO` cannot be resolved
    #    walking backward — the low-half instruction is met before the lui that gives it meaning.
    hi_val = {}
    for a in range(max(lo, add_a - window * 4), add_a, 4):
        i = ins.get(a)
        if i is None:
            continue
        _advance_immediate_constants(i, hi_val)
    base_candidates = [(value, reg) for reg, value in hi_val.items()
                       if reg in srcs and lo <= value < hi]
    if len(base_candidates) != 1:
        return None
    base, base_reg = base_candidates[0]

    # 3. the scale. The add's operands ARE the base and the scaled index, so the `sll` must write the
    #    OTHER addend — keying on "any sll writing either operand" picked up an unrelated `sll` on the
    #    base register once the window widened, giving idx == base and no cases.
    other = srcs[1] if srcs[0] == base_reg else srcs[0]
    idx_reg = shift = scale_op = scale_a = None
    for a in range(max(lo, add_a - window * 4), add_a, 4):
        i = ins.get(a)
        # `srl` as well as `sll`: the index is sometimes a BITFIELD, masked out with andi and shifted
        # DOWN into place (0x8004E5A4-0x8004E5A8 is `andi t2,v0,0xFF00 ; srl t2,t2,5`, i.e. bits 8-15
        # scaled by 8). An sll-only match missed that whole family.
        if i is not None and i.op in ("sll", "srl") and i.rd == other:
            idx_reg, shift, scale_op, scale_a = i.rt, i.shamt, i.op, a
    if idx_reg is None:
        return None
    # For a right-shifted bitfield the constant-index route is meaningless (the constants feeding it are
    # pre-shift field values), so the run walk is the only sound bound — it is unioned in below anyway.
    stride = (1 << shift) if scale_op == "sll" else 8

    # A low-bit mask immediately defining the scaled index is an exact bound: `andi idx,src,31`
    # means precisely 32 reachable slots. Prefer it over scanning for jump-shaped blocks, because
    # shared bodies after an inline trampoline table can also contain jumps and are not table slots.
    masked_count = None
    if scale_op == "sll":
        for a in range(scale_a - 4, max(lo, scale_a - window * 4) - 4, -4):
            i = ins.get(a)
            if i is None:
                continue
            if i.op == "andi" and i.rt == idx_reg:
                mask = i.imm
                if mask and (mask & (mask + 1)) == 0:
                    masked_count = mask + 1
                break
            if defines_reg(i, idx_reg):
                break

    if masked_count is not None:
        out = [base + k * stride for k in range(masked_count)]
        return out if all(not (t & 3) and lo <= t < hi for t in out) else None

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

    out = sorted(t for t in targets if not (t & 3) and lo <= t < hi)
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
# A DELAY SLOT BELONGS TO ITS CONTROL INSTRUCTION, NOT TO AN ADDRESS RANGE. When the next function
# entry starts ON the slot (the entry is a `jal` target in its own right, and the preceding function's
# `jr ra` fills its slot with that first instruction), the slot falls outside the owning function's
# [lo,hi) — and emitting nothing there silently DROPS a real instruction. Counted per module and
# printed with its denominator, because an emitter that quietly discards guest instructions is
# indistinguishable from one that had none to emit.
g_ds_borrowed = 0        # delay slots emitted from beyond the owning function's body
g_ds_unemittable = []    # (addr, category) — see the emit_module print for what each category means


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


def _branch_target_sources(ins, lo, hi):
    """{target_addr: [source_addrs]} for conditional branches inside [lo,hi)."""
    out = {}
    for a in range(lo, hi, 4):
        i = ins.get(a)
        if i is not None and i.kind == D.BRANCH and i.target is not None:
            out.setdefault(i.target, []).append(a)
    return out


# Single-source derivations that PRESERVE a value's identity: the result still designates the same
# projected vertex. Spyro's terrain renderer packs a vertex's clip code into the low bits with
# `sll rD,rS,5` + conditional `addi rD,rD,1|2|4|8`, and unpacks it later with `sra rD,rD,5`; a tap
# that treats any of those as "a different value now" sees the whole renderer as untapped.
# Deliberately EXCLUDES register-register arithmetic: `and s0,s0,a0` (the renderer's clip-code
# ACCUMULATOR) combines two values and designates neither.
_DERIVE_IMM_OPS = ("addi", "addiu", "andi", "ori", "xori")


def _derives_from(j, tracked):
    """If `j` derives a still-identifying value from a tracked register, return its dest, else None."""
    if j.kind in (D.SHIFT_I,) and j.rt in tracked:
        return j.rd                                     # sll/srl/sra rd, rt, sh
    if j.kind == D.ALU_RRI and j.op in _DERIVE_IMM_OPS and j.rs in tracked:
        return j.rt
    if j.kind == D.ALU_RRR and j.op in ("addu", "add", "or_", "or") and j.rt == 0 and j.rs in tracked:
        return j.rd                                     # `move rd, rs`
    return None


def _track_value(ins, lo, hi, start, seed, tsources, ds, limit=256):
    """Walk forward from `start`, following the value seeded in GPR `seed` through identity-preserving
    derivations, and return ([sw_addrs], n_skipped_delay_slot_stores).

    CROSSES CONDITIONAL BRANCHES ON PURPOSE. A forward branch inside this run just means the store may
    not execute — and a call that does not execute costs nothing. What is genuinely unsafe is arriving
    at a label reachable from OUTSIDE the run, because then the register state is not the one this walk
    reasoned about; so a branch TARGET is allowed only when every branch to it originates within the
    walk. Unconditional jumps and calls always stop it.

    DOES NOT STOP AT THE FIRST STORE. One projected vertex feeds several packet layouts in this engine
    (the FT3 and F3 arms write different offsets), and stopping early silently drops all but one.

    Tracking dies if the SEED register is redefined, even when the value also lives in a derived
    register, because the runtime slot that holds the vertex's depth is keyed by the seed."""
    tracked = {seed}
    stores, skipped = [], 0
    b = start + 4
    steps = 0
    while b < hi and tracked and steps < limit:
        steps += 1
        j = ins.get(b)
        if j is None:
            break
        # A label reachable from OUTSIDE this walk arrives with register state this walk never
        # reasoned about, so it stops here. `start - 4` counts as INSIDE: when the seed instruction
        # is itself a branch's delay slot, that branch sits at start-4 and is part of the same flow,
        # not a foreign entry.
        if b in tsources and any(src < start - 4 or src >= b for src in tsources[b]):
            break
        if j.kind == D.JUMP and j.op == "j" and j.target is not None \
                and lo <= j.target < hi and j.target > b:
            # AN UNCONDITIONAL FORWARD `j` INSIDE THIS FUNCTION IS NOT A BARRIER — it is a hop over
            # the other arm of an if/else, and the register state at the target is the state this
            # walk already reasoned about. Stopping here cost the stage-2 copies of two whole
            # renderers in Spyro (0x80026474->0x8002649C, 0x80026720->0x80026730,
            # 0x80026C00->0x80026C30, 0x80026CB8->0x80026CFC): depth was recorded into their
            # scratchpad caches and then never carried into the packet.
            # The delay slot still executes; process it, then resume at the target, where the
            # inbound-edge guard above is applied exactly as for any other address.
            slot = ins.get(b + 4)
            if slot is not None:
                if slot.kind == D.STORE and slot.op == "sw" and slot.rt in tracked:
                    stores.append(b + 4)
                else:
                    d = _derives_from(slot, tracked)
                    if d is not None:
                        tracked.add(d)
                    elif defines_reg(slot, seed):
                        break
                    else:
                        tracked -= {r for r in list(tracked) if defines_reg(slot, r)}
            b = j.target
            continue
        if j.kind in (D.JUMP, D.JUMPR):
            break
        if j.kind == D.BRANCH:
            slot = ins.get(b + 4)                        # the delay slot still executes
            if slot is not None:
                if slot.kind == D.STORE and slot.op == "sw" and slot.rt in tracked:
                    stores.append(b + 4)                 # emitted inside the control statement
                else:
                    d = _derives_from(slot, tracked)
                    if d is not None:
                        tracked.add(d)
                    elif defines_reg(slot, seed):
                        tracked.clear()
                    else:
                        tracked -= {r for r in list(tracked) if defines_reg(slot, r)}
            b += 8
            continue
        if j.kind == D.STORE and j.op == "sw" and j.rt in tracked:
            stores.append(b)     # delay-slot stores included: emit_run appends inside the
            b += 4               # control statement, which is exactly where they execute
            continue
        d = _derives_from(j, tracked)
        if d is not None:
            tracked.add(d)
        elif defines_reg(j, seed):
            break                                        # the seed's runtime slot is no longer valid
        else:
            tracked -= {r for r in list(tracked) if defines_reg(j, r)}
        b += 4
    return stores, skipped


def vertex_pz_stores(ins, lo, hi):
    """The native-depth taps for one function, as four maps:

        mfc2_holds   {mfc2_addr: (seed_gpr, z_reg)}   snapshot this vertex's Z at the read
        vertex_stores{sw_addr:   seed_gpr}            record that Z against the address written
        src_holds    {lw_addr:   seed_gpr}            remember where a loaded word came from
        copy_stores  {sw_addr:   seed_gpr}            carry any depth from there to the address written

    WHY BOTH HALVES EXIST. A game only gets native depth if the address the renderer looks up is the
    address the depth was recorded against. Spyro's terrain renderer projects vertices into a
    SCRATCHPAD cache (stage 1) and later assembles its GP0 packets from that cache (stage 2), so
    recording alone attaches depth to a buffer nothing draws from. Measured before this worked:
    depths at 0x1A86xx-0x1A8Dxx, packets drawn from 0x1AB7xx, every lookup a miss.

    WHY THE SOURCE ADDRESS IS HELD AT THE LOAD rather than rebuilt at the store: stage 2's load
    clobbers its own base register (`add t6,t6,s4` then `lw t6,0(t6)`), so the store site's version of
    that expression reads the loaded VALUE as a pointer and would attach an unrelated word's depth.

    Delay-slot stores are counted, not dropped quietly — the emitter embeds a delay slot inside its
    control statement and has nowhere to append the call."""
    ZPAIR = {12: 17, 13: 18, 14: 19, 15: 19}
    ds = {a + 4 for a in range(lo, hi, 4)
          if a in ins and ins[a].kind in (D.BRANCH, D.JUMP, D.JUMPR)}
    tsources = _branch_target_sources(ins, lo, hi)
    mfc2_holds, vertex_stores, src_holds, copy_stores = {}, {}, {}, {}
    skipped_ds = 0

    for a in range(lo, hi, 4):
        i = ins.get(a)
        if i is None:
            continue
        seed = None
        if i.kind == D.GTE_MOVE and i.op == "mfc2" and i.rd in ZPAIR:
            seed, kind = i.rt, "vertex"
        elif i.kind == D.LOAD and i.op == "lw":
            seed, kind = i.rt, "copy"
        if seed is None or seed == 0:
            continue
        stores, sk = _track_value(ins, lo, hi, a, seed, tsources, ds)
        skipped_ds += sk
        if not stores:
            continue
        if kind == "vertex":
            mfc2_holds[a] = (seed, ZPAIR[i.rd])
            for b in stores:
                vertex_stores[b] = seed
        else:
            src_holds[a] = seed
            for b in stores:
                if b not in vertex_stores:               # a real vertex store keeps its own depth
                    copy_stores[b] = seed
    return mfc2_holds, vertex_stores, src_holds, copy_stores, skipped_ds


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


def emit_func(exe, lo, hi, funcset, out, name, N, reentry=(), ra_computed=frozenset(),
              diagnostic_pcs=frozenset(), emitted_diagnostic_pcs=None):
    """Emit one C function: the proven LINEAR walk over the contiguous body [lo,hi), PLUS appended
    DUPLICATED tail blocks for shared-epilogue targets that live outside [lo,hi) (collect_tail_dups).
    The [lo,hi) emission is unchanged; the entry (lo) is always first; tails are reached only by goto.
    (A whole-function CFG flood-fill was tried and reverted — it mis-recompiled a register-jump-table
    state machine into an infinite loop; this is the additive, minimal version.)"""
    ins = {a: decode(a, exe.word(a)) for a in range(lo, hi, 4)}
    jt = find_jump_tables(exe, ins, lo, hi)
    ra_tails = ra_tail_returns(ins, lo, hi)
    # Native depth, mfc2 form: which `sw` stores a projected vertex, and which Z reg pairs with it.
    pz_holds, pz_stores, pz_srcs, pz_copies, pz_skipped_ds = vertex_pz_stores(ins, lo, hi)
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
    # INTRA-FUNCTION `jal` — a hand-written routine calling its OWN internal block (`bal`), which is
    # how the PSX libraries write a coroutine: `jal blk` … `blk: … jr $ra` inside one stack frame.
    # `demote_internal_labels` keeps such a block OUT of the function set, so the target is a label of
    # this body and the call cannot be a C call — `call_or_dispatch` would fail-fast on an address
    # that is not a function entry. Emit the guest's actual semantics instead: write the link, jump.
    #
    # The RETURN ADDRESS has to become a label too, because that is where the matching `jr $ra`
    # resumes. Those addresses are also the ONLY values such a `jr $ra` can legally hold, which is
    # what lets it be emitted as a switch over labels rather than a runtime dispatch.
    intra_links = {a2: ins[a2].target for a2 in standalone
                   if ins[a2].op == "jal" and ins[a2].target in ins
                   and ins[a2].target not in funcset and ins[a2].target in standalone}
    labels |= set(intra_links.values())
    ra_conts = sorted({a2 + 8 for a2 in intra_links if (a2 + 8) in standalone})
    labels |= set(ra_conts)
    # A computed `jr $ra` that resumes at an EARLIER address closes a loop exactly as a backward
    # branch does — the decoder's own strip loop is one — so it needs the same host-turn gate.
    computed_jrs = [a2 for a2 in standalone
                    if a2 in ra_computed and ins[a2].op == "jr" and ins[a2].rs == 31]
    backedges |= {t for t in ra_conts if any(t <= j for j in computed_jrs)}

    def emit_run(s, e):
        # Advance deterministic guest instruction-time by the instructions that actually ran, batching each
        # straight-line block into one call. A debugger pause or slow host therefore advances ZERO
        # guest time. Flush before a label so a goto skips the preceding fall-through block's count;
        # control+delay-slot ticks are spliced after the delay slot and before the transfer.
        block_ticks = 0

        def flush_ticks():
            nonlocal block_ticks
            if block_ticks:
                out.append(f"  rec_guest_instruction_ticks(c, {block_ticks}u);")
                block_ticks = 0

        a = s
        while a < e:
            i = ins[a]
            if a in labels:
                flush_ticks()
                out.append(f"L_{a:08X}:;")
                # Loop back-edge: give the host a turn. One predictable load-and-test per iteration,
                # and only on loops — see the `backedges` note above for why function entry alone is
                # not enough.
                if a in backedges:
                    out.append("  if (c->pending_work) rec_irq_poll(c);")
            # A checkpoint observes entry to the guest instruction, including a branch landing on
            # this label. Placing it before the label lets `goto L_PC` skip the observer entirely.
            checkpoint_keys = [(owner, pc) for owner, pc in diagnostic_pcs
                               if pc == a and (owner is None or owner == lo)]
            if checkpoint_keys:
                out.append(f"  pc_observer_at(c, 0x{a:08X}u);")
                if emitted_diagnostic_pcs is not None:
                    for key in checkpoint_keys:
                        emitted_diagnostic_pcs[key] = emitted_diagnostic_pcs.get(key, 0) + 1
            if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                global g_ds_borrowed
                slot = ins.get(a + 4)
                if slot is None and a + 4 < exe.text_end:
                    # BORROW IT. The hardware runs this word before the transfer no matter which
                    # function the address list says owns it, so the only faithful emission is the
                    # instruction itself. (A borrowed slot is outside the pz_* maps computed over
                    # [lo,hi), so a vertex store sitting here is not depth-tapped — g_ds_borrowed is
                    # what makes that visible rather than silent.)
                    slot = decode(a + 4, exe.word(a + 4))
                    g_ds_borrowed += 1
                if slot is None or slot.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                    # Off the end of the image, or a control instruction in a slot — a shape the
                    # hardware does not define, so in practice a word of DATA being walked as code.
                    # Nothing faithful to emit; SAY so rather than emitting silence.
                    g_ds_unemittable.append((a + 4, "off-image" if slot is None else "control"))
                    ds_c = "/* DS */"
                else:
                    if slot.kind == D.UNKNOWN:
                        g_ds_unemittable.append((a + 4, "undecodable"))
                    ds_c = emit_simple(slot)
                # NATIVE-DEPTH TAPS FOR A DELAY-SLOT INSTRUCTION. A delay slot is emitted INSIDE the
                # control statement, so a tap appended after that statement would run on the wrong
                # path — or not at all. Splicing it into the delay-slot text puts it exactly where
                # the hardware runs it: after the slot executes, before the transfer. Hand-written
                # assembly fills delay slots with real work; 96 vertex stores in this game's main
                # executable sit here, and four whole renderers are lost to one such store each.
                sa = a + 4
                if slot is not None:
                    if sa in pz_srcs:                   # the hold must precede its own load
                        ds_c = f"gte_hold_src(c, {pz_srcs[sa]}, {addr_expr(slot)}); " + ds_c
                    if sa in pz_holds:
                        g, z = pz_holds[sa]
                        ds_c += f" gte_hold_pz(c, {g}, {z});"
                    if sa in pz_stores:
                        ds_c += f" gte_record_pz(c, {addr_expr(slot)}, {pz_stores[sa]});"
                    elif sa in pz_copies:
                        ds_c += f" gte_copy_pz(c, {pz_copies[sa]}, {addr_expr(slot)});"
                delay_body = ds_c
                ds_c += f" rec_guest_instruction_ticks(c, {block_ticks + 2}u);"
                block_ticks = 0
                out.extend(emit_control(i, ds_c, funcset, labels, N, jt.get(a), a in ra_tails,
                                        a in ra_computed, intra_links.get(a), ra_conts))
                if (a + 4) in ds_label_targets:        # the delay slot is also a branch target
                    out.append(f"  goto L_DSAFTER_{a:08X};")
                    out.append(f"L_{a + 4:08X}:; {delay_body} rec_guest_instruction_ticks(c, 1u);")
                    out.append(f"L_DSAFTER_{a:08X}:;")
                a += 8
            else:
                # NATIVE DEPTH, the copy source: capture WHERE this word comes from BEFORE the load
                # runs. `lw t6,0(t6)` overwrites the very register the address is built from, so a
                # capture placed after it records the loaded VALUE as if it were an address and would
                # carry an unrelated word's depth — a wrong depth, which is worse than none. Emitting
                # it here is the entire guarantee; one line later silently reintroduces the bug.
                if a in pz_srcs:
                    out.append(f"  gte_hold_src(c, {pz_srcs[a]}, {addr_expr(i)});")
                st = emit_simple(i)
                if st:
                    out.append("  " + st)
                # NATIVE DEPTH (mfc2 form): snapshot this vertex's Z at the read, and record it against
                # the address written. Both are emitted AFTER their instruction, so the guest's own
                # memory write and register update are untouched and the tap is purely additive.
                if a in pz_holds:
                    g, z = pz_holds[a]
                    out.append(f"  gte_hold_pz(c, {g}, {z});")
                if a in pz_stores:
                    out.append(f"  gte_record_pz(c, {addr_expr(i)}, {pz_stores[a]});")
                elif a in pz_copies:
                    out.append(f"  gte_copy_pz(c, {pz_copies[a]}, {addr_expr(i)});")
                block_ticks += 1
                a += 4
        flush_ticks()

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
            else f"{N.router}(c, {addr_const(target)});")



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


def ra_const_base(exe, at, reg):
    """The LINK-TIME CONSTANT address held in `reg` at instruction `at`, or None if it is not one.

    Only the `lui rX,H` [`addiu rX,rX,L`] idiom, scanned back at most 16 instructions. Anything else
    — a base arriving in an argument register, a pointer loaded from memory, an indexed address — is
    UNRESOLVED and returned as None, never guessed in either direction. `ra_computed_jumps` counts
    the unresolved cases and prints the count, because "found no save slot" and "could not look" are
    different answers and must not read the same."""
    lo_add = None
    for x in range(at - 4, max(at - 68, exe.load) - 4, -4):
        i = decode(x, exe.word(x))
        if i.op == "addiu" and i.rt == reg and lo_add is None and i.rs == reg:
            lo_add = i.simm
        elif i.op == "lui" and i.rt == reg:
            return ((i.simm & 0xFFFF) << 16) + (lo_add or 0)
        elif i.rt == reg and i.kind in (D.LOAD, D.ALU_RRI) and i.op != "addiu":
            return None
        elif i.rd == reg and i.kind in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V):
            return None
    return None


def ra_global_save_slots(exe):
    """Every `(link-time base, offset)` in the MODULE that some `sw $ra` writes to.

    MODULE-WIDE ON PURPOSE, and keyed by the ADDRESS rather than the displacement. A frameless
    function that parks its return address in a fixed GLOBAL block shares that block with every other
    frameless function by construction, so "the same *fragment* stored to this offset" is not a
    property the guest has — see `ra_computed_jumps`. Stack slots are excluded: an `sp`-relative
    displacement means nothing without the frame it belongs to, and `lw $ra, N(sp)` is already a
    return by the rule above."""
    slots = set()
    for a in range(exe.load, exe.text_end, 4):
        i = decode(a, exe.word(a))
        if i.op == "sw" and i.rt == 31 and i.rs != 29:
            b = ra_const_base(exe, a, i.rs)
            if b is not None:
                slots.add((b, i.simm))
    return slots


def ra_computed_jumps(exe, bounds, stats=None):
    """Addresses of `jr $ra` that are COMPUTED JUMPS, not returns — decided per-`jr` by
    reaching-definitions on `$ra` over each guest function in `bounds`.

    `jr $ra` normally means "return", and the emitter renders it as `return;`. That is wrong for
    hand-written assembly that uses `jal`/`jr $ra` as an INTERNAL COROUTINE mechanism: the `$ra` it
    reads is a mid-body resume point, so `return;` unwinds out of the function instead of resuming,
    dropping the rest of the routine's work (and, where the function allocated a frame, its epilogue).

    Spider-Man's `0x8002A338` is the case this was built for — a resumable bit-stream decoder that
    saves its whole register context, INCLUDING its continuation, to a global block and restores it on
    re-entry. Its three `jr $ra` are not the same kind: `0x8002A460` jumps to a resume point, while
    `0x8002A7E0` and `0x8002A838` are ordinary returns after the frame reload. A whole-body gate ("this
    body writes $ra as data, so convert all of its `jr $ra`") is therefore wrong whichever way it
    answers; the decision has to be per-`jr`.

        reaching def is `lw $ra, N(sp)`                              -> return
        reaching def is `lw $ra, N(<const base>)` and some `sw $ra`
          in the MODULE writes that same (base, N)                   -> return  (a save slot)
        reaching def is `lw $ra, N(<const base>)` otherwise          -> COMPUTED (a continuation)
        reaching def is `lw $ra, N(<unresolved base>)`               -> return  (cannot prove either)
        reaching def is a `jal` / `bltzal` / `bgezal` / `jalr $ra`   -> COMPUTED (a live link)
        anything else, no definition, or DISAGREEING paths           -> return

    The default is `return` on everything it cannot prove, so this can only move a site AWAY from
    current behaviour on positive evidence.

    TWO THINGS DECIDE THIS, AND BOTH WERE WRONG WHEN IT FIRST LANDED (spyro docs/issues/0040, 0046 —
    nine ordinary returns dispatched as coroutines, a `recomp-MISS` at 0x8001E91C and SIGABRT 3544
    frames into the run, on a game that has no coroutine anywhere in its MAIN).

    **The save-slot test is MODULE-WIDE and keyed by the ADDRESS.** A frameless function that parks
    `$ra` in a GLOBAL block shares that block with every other frameless function BY CONSTRUCTION —
    Spyro's hand-written renderers all spill to `0x80077DD8+44` (19 stores, 457 `sw $ra` sites in the
    module). So "did THIS fragment also store to that displacement" is not a property the guest has:
    it answers "continuation" on a plain return whenever the partition splits a body, and it answers
    "return" on a real continuation whenever an unrelated block happens to reuse the displacement.
    `ra_global_save_slots` asks the question the guest actually answers. It also makes the analysis
    INDEPENDENT of the partition, which matters because the partition is not the guest function list
    and never can be: it is every address the emitter had to make addressable.

    **The traversal is a fixpoint over the BASIC-BLOCK GRAPH, not a sweep in address order.** A `jal`
    on a path that does not reach the `jr` cannot define the `$ra` that `jr` reads. Spyro
    `0x80053570` is the case: the `bne` at `0x8005358C` jumps over the `jal` at `0x80053598`, whose
    path leaves through its own `jr $a3`, so `$ra` is untouched on every path that reaches
    `0x800535E0`/`0x80053600`. A linear sweep walks past that `jal` anyway. Paths that DISAGREE merge
    to "not proven", i.e. return.

    Walking the graph means deciding what a `jal` does, and the answer is taken from `emit_func`'s own
    `intra_links` rule so the classifier and the emitter cannot drift: a `jal` whose target lies
    INSIDE this fragment and is NOT a function entry is emitted `goto L_target` (an internal `bal`),
    so control really does enter that block with the link live — walk into it. A `jal` to a FUNCTION is
    a C call whose callee returns here, so do not. `bltzal`/`bgezal` are always emitted as calls
    (`emit_control`), so they are never walked into either.

    Spider-Man's `0x8002A338` is the case the analysis exists for — a resumable bit-stream decoder
    whose three `jr $ra` are not the same kind: `0x8002A460` resumes a continuation, `0x8002A7E0` and
    `0x8002A838` are ordinary returns. It stays classified correctly under both rules above, and it is
    worth seeing WHY the save-slot rule does not misfire on it: the decoder does save its continuation
    to a global (`0x80097D88+48`), but it saves it from `$at` (`or $1,$zero,$ra` at `0x8002A7EC`, then
    `sw $at,48($t6)`), never from `$ra` — because at that instant `$ra` holds the CALLER's return
    address, which it restores from the stack and returns through. A callee-save prologue stores `$ra`
    itself. That difference is not a coincidence, it is what the two idioms mean, and it is why
    `sw $ra` is the right thing to look for. Measured: 0 `sw $ra` anywhere in Spider-Man's MAIN
    targets `(0x80097D88, 48)`, out of 1039 `sw $ra` sites.

    `bounds` is the function-start list to partition by; mid-body re-entry seeds must be excluded from
    it (`emit_module` does). `stats`, if given, is filled with the denominators the caller prints. The
    game-side tool `spyro/tools/ra_classes.py` cross-checks this set off `generated/` without a build.
    """
    slots = ra_global_save_slots(exe)
    fset = set(bounds)
    unresolved = set()
    n_jr = 0

    def defines(i):
        """What instruction `i` leaves in `$ra`: 'return', 'computed', or None (does not touch it)."""
        if i.op == "lw" and i.rt == 31:
            if i.rs == 29:
                return "return"
            b = ra_const_base(exe, i.addr, i.rs)
            if b is None:
                unresolved.add(i.addr)
                return "return"          # cannot prove a continuation -> the stated default
            return "return" if (b, i.simm) in slots else "computed"
        if i.op in ("jal", "bltzal", "bgezal"):
            return "computed"
        if i.kind == D.JUMPR and i.op == "jalr" and i.rd == 31:
            return "computed"
        if (i.kind in (D.ALU_RRR, D.SHIFT_I, D.SHIFT_V) and i.rd == 31) \
                or (i.kind in (D.ALU_RRI, D.LUI) and i.rt == 31):
            return "return"
        return None

    out = set()
    for idx, lo in enumerate(bounds):
        hi = bounds[idx + 1] if idx + 1 < len(bounds) else exe.text_end
        at_jr = {}                        # jr-$ra address -> set of states it is REACHED with
        work = [(lo, None)]
        seen = set()
        while work:
            a, st = work.pop()
            if not (lo <= a < hi) or (a, st) in seen:
                continue
            seen.add((a, st))
            i = decode(a, exe.word(a))
            if i.kind == D.UNKNOWN:
                continue                  # trailing data, not code
            if i.op == "jr" and i.rs == 31:
                at_jr.setdefault(a, set()).add(st)
                continue                  # the `jr` ends the path
            nxt = defines(i) or st
            if i.kind in (D.BRANCH, D.JUMP, D.JUMPR):
                # ORDER IS THE HARDWARE'S: a branch-and-link writes `$ra` when it ISSUES and the delay
                # slot runs after, so a delay slot that also writes `$ra` overrides the link.
                if i.op in ("jal", "bltzal", "bgezal"):
                    nxt = "computed"
                ds = defines(decode(a + 4, exe.word(a + 4)))
                if ds is not None:
                    nxt = ds
                if i.kind == D.JUMPR:
                    if i.op == "jalr":
                        work.append((a + 8, nxt))
                    continue              # an unresolved register jump ends this path
                if i.op == "jal":
                    if lo <= i.target < hi and i.target not in fset:
                        work.append((i.target, nxt))     # internal `bal` — emit_func's intra_links
                    work.append((a + 8, nxt))
                elif i.op in ("bltzal", "bgezal"):
                    work.append((a + 8, nxt))            # always a call; resumes past the slot
                else:
                    if i.target is not None:
                        work.append((i.target, nxt))
                    if i.kind == D.BRANCH:
                        work.append((a + 8, nxt))
                continue
            work.append((a + 4, nxt))
        for a in range(lo, hi, 4):
            i = decode(a, exe.word(a))
            if i.op == "jr" and i.rs == 31 and i.kind != D.UNKNOWN:
                n_jr += 1
        for site, states in at_jr.items():
            if states == {"computed"}:    # proven on EVERY path that reaches it
                out.add(site)
    if stats is not None:
        stats.update(jr_sites=n_jr, fragments=len(bounds), save_slots=len(slots),
                     unresolved=sorted(unresolved))
    return out


def emit_control(i, ds_c, funcset, labels, N, jtargets=None, ra_tail=False, ra_computed=False,
                 intra_link=None, ra_conts=()):
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
            L.append(f"  {{ int _t = ({cond}); {R(31)} = {addr_const(i.addr + 8)}; {ds_c} "
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
            L.append(f"  {R(31)} = {addr_const(i.addr + 8)};")
            if intra_link is not None:
                # INTERNAL call (`bal`): the target is a label of THIS body, not a function. Write the
                # link and jump — a C call would need a callee that does not exist, and dispatching
                # the address fail-fasts because a mid-function address is not an entry.
                L.append(f"  {ds_c} goto L_{intra_link:08X};   /* internal call (bal) */")
            else:
                L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset, N)}")
        else:  # j
            if i.target in labels:
                L.append(f"  {ds_c} goto L_{i.target:08X};")
            else:
                L.append(f"  {ds_c} {call_or_dispatch(i.target, funcset, N)} return;")
        return L
    # JUMPR
    if i.op == "jr":
        if i.rs == 31 and ra_computed:
            # COROUTINE RESUME, not a return — `$ra` here provably holds a mid-body resume point
            # rather than a caller's return address (ra_computed_jumps decided this statically, per
            # `jr`, by reaching-definitions). Route it like any other computed jump: the target is a
            # mid-function address, so it must be seeded as a re-entry point for the router to
            # resolve it — a missing one fail-fasts with a [recomp-MISS] naming the address, which is
            # the diagnosable failure, not a silent one. Same target-latching rule as the computed
            # `jr` below: MIPS reads the register at the JUMP, before the delay slot runs.
            #
            # The legal targets are known STATICALLY and exhaustively: `$ra` here was written by one
            # of this body's own internal `jal`s, so it is one of their return addresses — each of
            # which emit_func has made a label. A switch over those keeps the jump inside this C
            # function, which is the whole point; routing it out through the dispatcher instead would
            # need every resume point seeded as a mid-function entry, and a missing seed shows up as
            # a recomp-MISS on decoder DATA (0x03FF03FF — measured, and why the first attempt at this
            # was reverted at 88f58d7f). `default` still dispatches, so an unforeseen value fails
            # fast and NAMES itself rather than being silently swallowed by a `return`.
            cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};"
                             for t in ra_conts if t in labels)
            if cases:
                L.append(f"  {{ uint32_t _tgt = {R(31)}; {ds_c} switch ({link_space('_tgt')}) {{ "
                         f"{cases} default: {N.router}(c, _tgt); return; }} }}"
                         f"   /* coroutine resume, not a return */")
            else:
                L.append(f"  {{ uint32_t _tgt = {R(31)}; {ds_c} {N.router}(c, _tgt); return; }}"
                         f"   /* coroutine resume, not a return */")
        elif i.rs == 31:
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
            L.append(f"  {{ {ds_c} switch ({link_space(R(i.rs))}) {{ {' '.join(cases)} "
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
            # LATCH THE TARGET BEFORE THE DELAY SLOT. MIPS reads the jump register at the JUMP, not
            # after its delay slot, so a delay slot that overwrites that register does NOT change
            # where control goes. Restoring the jump register in the delay slot is a legal and
            # common idiom precisely because of this. Reading it after `ds_c` would jump to whatever
            # the delay slot happened to leave there.
            L.append(f"  {{ uint32_t _tgt = {R(i.rs)}; {ds_c} {N.router}(c, _tgt); return; }}")
    else:  # jalr rd, rs
        # Two MIPS details, both of which this emitted wrongly:
        #
        # 1. rd may be $zero. `jalr $zero, rs` is a legal encoding (a call whose return address is
        #    discarded), and writing it directly emitted `c->r[0] = <addr>` — CORRUPTING THE ZERO
        #    REGISTER. Every `addiu rX, $zero, imm` in the substrate reads r0, so the damage is
        #    immediate, unbounded and nowhere near the jump. wr() is the helper that discards r0
        #    writes; every other write site already uses it.
        # 2. The target register is read at the JUMP, before the delay slot runs — same latching rule
        #    as the computed `jr` above.
        L.append(f"  {{ uint32_t _tgt = {R(i.rs)}; {wr(i.rd, addr_const(i.addr + 8))} {ds_c} "
                 f"{N.router}(c, _tgt); }}")
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


def code_image_stats(exe):
    """(undecodable-word fraction, jr-ra count, stack-prologue count) over the whole image — the
    evidence looks_like_code reports. Only the jr-ra count decides (see there); the rest is context
    for the diagnostic."""
    lo, hi = exe.load, exe.text_end
    n = (hi - lo) // 4
    unk = ret = prol = 0
    for a in range(lo, hi - 3, 4):
        w = exe.word(a)
        if decode(a, w).kind == D.UNKNOWN:
            unk += 1
        if w == 0x03E00008:                          # jr ra
            ret += 1
        if (w & 0xFFFF8000) == 0x27BD8000:           # addiu sp, sp, -N
            prol += 1
    return (unk / n if n else 1.0), ret, prol


def looks_like_code(exe):
    """Is this image recompilable CODE — or data that merely passed through a code slot?

    An overlay slot is a load DESTINATION, not a promise that what lands there is code: games stream
    data through the same arena their code overlays use, and a scanner that records every arena load
    (Spyro's tools/overlay_scan.py) hands the recompiler data reads alongside code — 512KB, 480KB and
    75KB ones among Spyro's twelve. Recompiling data is not a small error: with no `jr ra` anywhere
    in the image, every seeded "function" flood-fills to the module end, and the flood-fill's
    shared-tail duplication then re-emits the whole module PER FUNCTION — 480KB of Spyro data became
    ~144MB of garbage C (one shard alone was 83MB). Asked independently, Ghidra's analyser finds 0
    functions / 0% instruction coverage in those images — the recompiler now refuses to out-guess it.

    The criterion is the presence of a function RETURN (`jr ra`, the exact word 0x03E00008), and
    nothing else, because every broader signal misfires on a real input:
      * undecodable-fraction: real overlays mix data in — Tomba!2's area overlays are 20-30%
        undecodable with HUNDREDS of returns (code + embedded graphics/tables); Spyro's data reads
        include nop-padding runs that decode 100% clean. Neither direction separates.
      * stack-prologue pattern: 2^-17 per random word — a 480KB data image expects ~1 spurious hit
        (Spyro's OV_20F800). Too weak to vouch for code.
      * `jr ra` exact-word: 2^-32 per random word — 3e-5 over the largest overlay seen. Data never
        has one; every callable function ends in one.
    The one real-code shape with NO return is a noreturn stage main (Tomba!2 START.BIN: a filename
    table with one stage fn that never `jr ra`s) — the caller overrides this test with the game's
    EXPLICIT seeds (someone vouched for an entry point), so the absence of returns alone can never
    silently drop a seeded overlay.
    Pinned by test_emit.py's data-image guard tests."""
    return code_image_stats(exe)[1] > 0


def overlay_funcs(exe, overlay_dir, base=None):
    """Seed resident functions that are reached ONLY from the stage overlays (\\BIN\\*.BIN, loaded
    raw to `base` at runtime). Those overlays `jal` into resident MAIN.EXE functions (the cooperative
    scheduler, file loaders, etc.) that direct-jal discovery within MAIN.EXE never sees. We scan each
    overlay's words for `jal` targets landing in MAIN.EXE text whose first word decodes as a real
    instruction, and seed them; discover_funcs then follows their call graph. A target that is the
    resident control instruction immediately before it is a DELAY SLOT, however, can never be a
    separate function entry: the control instruction executes that word as part of its own flow.
    Mixed code/data overlays can contain jal-shaped payload words, and promoting one of those delay
    slots splits the resident body before the slot so the real branch loses it. Reject that
    structurally impossible class here; this is independent of prologue style and therefore retains
    legitimate stackless resident callees. Pure binary input (the overlays are game data, like
    MAIN.EXE) — no Ghidra. Skipped if absent.

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
                previous = decode(ins.target - 4, exe.word(ins.target - 4)) \
                    if ins.target - 4 >= lo else None
                is_delay_slot = previous is not None and previous.kind in (
                    D.BRANCH, D.JUMP, D.JUMPR)
                if not is_delay_slot and decode(ins.target, exe.word(ins.target)).kind != D.UNKNOWN:
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


LINKING_OPS = ("jal", "bltzal", "bgezal")


def demote_internal_labels(exe, funcs, keep):
    """Remove entries that are INTERNAL LABELS of a larger guest function, not functions.

    `jal` discovery is deliberately eager, and hand-written assembly breaks it: a routine that uses
    `jal`/`jr $ra` as an internal coroutine mechanism inside ONE stack frame gets its internal block
    promoted to a function entry, which splits the real body. The emitter then renders an
    intra-function branch to that block as `call + return`, so the enclosing function exits without
    reaching its epilogue and leaks its frame — Spider-Man `0x8002A338` leaks 4 bytes per call,
    measured.

    RULE 1 IS A PROOF, NOT A HEURISTIC. An entry reached by a NON-LINKING branch (`bgez`, `j`, `beq`
    …) from inside another body **cannot** be a function: control arrives there with no return
    address established, so it has no way to return. It is an internal label by construction.

    This is what four earlier heuristics missed. They tested "no prologue AND fall-through reachable
    AND same host" — but NONE of the candidates has a prologue, so that test has no discriminating
    power at all, and tuning it demoted 78, then 325, then 50 entries. Worse, it took genuine library
    functions with it (`StGetNext`), which the port itself super-calls, breaking the LINK.

    Measured on Spider-Man: 5 entries are branch targets from another body, and rule 1 separates them
    exactly — because the distinction it draws is the real one.

        0x8002A478   bgez x14 + j x1 + jal x3   MIXED     -> internal label   (demote)
        0x8007CD44   bgezal x2                  \\
        0x8007D160   bltzal x8                   |  LINK-ONLY -> real subroutines (KEEP). These are
        0x8007D1F0   bltzal x2                   |  branch-and-link targets that discover_funcs
        0x8007D254   bltzal x2                  /   seeds ON PURPOSE; demoting one reintroduces the
                                                    very stack leak this function exists to fix.

    THERE IS DELIBERATELY NO SECOND RULE. A "link-only, single host, no epilogue of its own" fixpoint
    was written to also catch `0x8002A5F4` (reached by `jal` x4 from inside the same body) and it
    demoted **255** entries, `StGetNext` among them — reproducing attempt 1's link breakage exactly.
    The flaw: a LEAF function has no epilogue *because it needs none*, returning straight through
    `jr $ra` with the caller's value, so "no epilogue" is true of a huge class of perfectly ordinary
    functions and carries almost no information. It did not even catch its intended target, whose
    region does contain the shared tail's `lw ra, (sp)`.

    That is the same mistake as the prologue heuristic, one level along: pairing a proof with a guess
    and letting the guess do the work. Rule 1 alone is exact here. If a future body genuinely needs
    more, derive another PROOF — do not tune a threshold.

    `keep` is never demoted (explicit seeds / real call targets from outside).
    """
    import bisect as _bisect
    lo, hi = exe.load, exe.text_end
    keep = set(keep)

    def has_prologue(a):                       # `addi(u) sp, sp, -N` — allocates its OWN frame
        return (exe.word(a) & 0xFFFF8000) in (0x27BD8000, 0x23BD8000)

    demoted = set()
    for _ in range(16):
        live = sorted(set(funcs) - demoted)
        live_set = set(live)
        # references into each entry, attributed to the body they come from
        refs = {}
        for idx, a in enumerate(live):
            end = live[idx + 1] if idx + 1 < len(live) else hi
            for x in range(a, end, 4):
                i = decode(x, exe.word(x))
                if i.kind == D.UNKNOWN:
                    break                      # trailing data, not code
                if i.kind in (D.BRANCH, D.JUMP) and i.target is not None and lo <= i.target < hi:
                    refs.setdefault(i.target, []).append((i.op, i.kind, a))
        grew = False
        for t, rs in refs.items():
            if t in demoted or t in keep or t not in live_set or has_prologue(t):
                continue
            # A CONDITIONAL, non-linking arrival from a DIFFERENT body. Conditional is load-bearing:
            # the branch's FALL-THROUGH continues in that same body, so both successors belong to one
            # function and the target is part of its control flow, not a callee.
            #
            # An unconditional `j` does NOT qualify, and assuming it did was wrong. `j` into a
            # function is the ordinary TAIL-CALL idiom — `$ra` still holds the caller's return
            # address, so the target returns perfectly well. Spider-Man 0x8007D534 is exactly that:
            # `jal` x5 then `j` x2 from the same two bodies (call it, then tail-jump for the last
            # one). Counting `j` demoted it and would have inlined a real function into two callers.
            if any(kind == D.BRANCH and op not in LINKING_OPS and h != t for op, kind, h in rs):
                demoted.add(t)
                grew = True
        if not grew:
            break
    return demoted


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


def write_if_changed(path, text):
    """Write `text` to `path` only when it DIFFERS from what is already there.

    THE POINT IS BUILD TIME, and it is the single biggest tax on this workflow. The substrate is ~200
    MB of generated C across ~47 translation units; recompiling all of it takes 10-15 minutes, while a
    no-op build links in about 3 seconds. Rewriting a file with byte-identical content still bumps its
    mtime, so make rebuilds it — which means ANY emitter change, however narrow, has cost a full
    rebuild even when it altered two shards out of forty-seven.

    Correctness note: this is safe precisely because the content is the build input. If the bytes are
    the same, the object file would be the same; nothing downstream can tell the difference. It is not
    a cache and cannot go stale — there is no key to get wrong."""
    try:
        if open(path, "r").read() == text:
            return False
    except OSError:
        pass
    open(path, "w").write(text)
    return True


def emit_module(exe, out_dir, N, seeds, ov_dir=None, limit=None, shards=8, soft_seeds=None, reentry=(),
                diagnostic_pcs=frozenset()):
    """Discover the recompiled function set for `exe` and emit its module (shards + dispatch TU +
    decls header) under the symbol/file names in `N`. Shared by the MAIN.EXE module and the boot-
    stub module; the stub gets distinct names so its func_<addr> don't collide with MAIN's."""
    diagnostic_pcs = {(None, item) if isinstance(item, int) else tuple(item)
                      for item in diagnostic_pcs}
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
    # DEMOTE INTERNAL LABELS. `jal` discovery is eager, and hand-written assembly using `jal`/`jr $ra`
    # as an internal coroutine inside ONE frame gets its inner block promoted to a function entry,
    # splitting the real body. The emitter then renders the routine's own back-edge — an unconditional
    # intra-function branch — as `call + return`, so the function exits without its epilogue. Measured
    # live on Spider-Man 0x8002A338 (libpress `DecDCTvlc`): returns with sp 4 low and $ra holding its
    # internal link 0x8002A424, its caller's locals wrecked, and the intro FMV decodes one strip per
    # movie and stops. See demote_internal_labels for the criterion and for the three lookalikes it
    # must spare.
    internal = demote_internal_labels(exe, funcs, keep=seeds)
    if internal:
        funcs = [f for f in funcs if f not in internal]
        print(f"[{N.wrap}] demoted {len(internal)} internal label(s) back into their guest function: "
              + " ".join(f"0x{a:08X}" for a in sorted(internal)))
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
    write_if_changed(os.path.join(out_dir, N.decls), "\n".join(hdr) + "\n")

    # COROUTINE `jr $ra` sites, decided ONCE over the whole module. The partition is the function set
    # with the mid-body RE-ENTRY seeds removed: a re-entry point splits a guest function into
    # fragments, and the `jal` whose link a later `jr $ra` reads then lives in a DIFFERENT fragment —
    # so a per-fragment analysis finds no definition and silently answers "return", which is the
    # right-looking answer for the wrong reason. See ra_computed_jumps.
    ra_stats = {}
    ra_computed = ra_computed_jumps(exe, [a for a in funcs if a not in set(reentry)], ra_stats)
    # PRINTED UNCONDITIONALLY, WITH ITS DENOMINATORS. "0 coroutines" is the answer for most games and
    # it has to be distinguishable from "the analysis never ran" — and from "it ran but could not see".
    print(f"[{N.wrap}] `jr $ra`: {len(ra_computed)} of {ra_stats['jr_sites']} emitted as a COMPUTED "
          f"JUMP (coroutine resume) over {ra_stats['fragments']} fragments; "
          f"{ra_stats['save_slots']} global `sw $ra` save slots; "
          f"{len(ra_stats['unresolved'])} `lw $ra` base(s) UNRESOLVED (blind spot — counted as "
          f"returns): " + (" ".join(f"0x{a:08X}" for a in sorted(ra_computed)) or "(none computed)"))

    shard = [["// GENERATED — DO NOT EDIT.", f'#include "{N.decls}"', ""] for _ in range(shards)]
    emitted_diagnostic_pcs = {}
    for k, a in enumerate(funcs):
        emit_func(exe, a, nxt_of[a], funcset, shard[k % shards], f"{N.gen}_{a:08X}", N, reentry,
                  ra_computed, diagnostic_pcs, emitted_diagnostic_pcs)
    requested = set(diagnostic_pcs)
    missing = sorted((key for key in requested if emitted_diagnostic_pcs.get(key, 0) == 0),
                     key=lambda key: (-1 if key[0] is None else key[0], key[1]))
    duplicates = sorted((key for key in requested if emitted_diagnostic_pcs.get(key, 0) > 1),
                        key=lambda key: (-1 if key[0] is None else key[0], key[1]))
    extra = sorted(set(emitted_diagnostic_pcs) - requested)
    emitted_sites = sum(emitted_diagnostic_pcs.values())
    if missing or duplicates or extra or emitted_sites != len(requested):
        def checkpoint_name(key):
            owner, pc = key
            return f"{'*' if owner is None else f'0x{owner:08X}'}@0x{pc:08X}"
        raise SystemExit(f"[recomp] diagnostic checkpoints requested_targets={len(requested)} "
                         f"emitted_sites={emitted_sites} missing="
                         f"{[checkpoint_name(x) for x in missing]} "
                         f"duplicate_checkpoints="
                         f"{[checkpoint_name(x) + f'({emitted_diagnostic_pcs[x]})' for x in duplicates]} "
                         f"extra={extra}")
    for s in range(shards):
        write_if_changed(os.path.join(out_dir, f"{N.shardpfx}_{s}.c"), "\n".join(shard[s]) + "\n")

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
        # OT/GTE SUBMISSION ATTRIBUTION ON DIRECT CALLS (docs/plans/graphics-producer-db.md, guest leg).
        # Every direct call is emitted as this wrapper (call_or_dispatch) and the dispatch switch calls it
        # too, so ONE push/pop here covers every guest call of BOTH kinds. Before this, the attribution
        # stack was pushed only around INDIRECT dispatch, and the packet-pool stores are performed by
        # shared SDK-adjacent routines reached by direct jal — so the stack was empty for exactly the
        # stores that mattered and the guest leg attributed 1.61% of its prims.
        #
        # PRICED, NOT ASSUMED: 3,071,077 wrapper calls over a 1200-frame replay (2,559/frame), and an arm
        # that pushes a CONSTANT (identical downstream behaviour, mechanism fully paid) measured +0.0%
        # user CPU against baseline over interleaved pairs with PSXPORT_NOPACE=1. The wrapper already
        # stores c->pc and does two load-and-test branches, so this is the same order of work as what it
        # already pays. Secondary, and the real cost: .text grows ~4.5% because the wrapper loses its
        # tail call into the body (it must return in order to pop).
        #
        # RESTORE, NOT CLEAR, and this is why the stack exists at all: c->pc is the same word one level
        # down and is NOT restored on return, so a c->pc-based guest leg can only ever name a CALLEE.
        # That is precisely how the removed PC route scored 100% coverage and ~0% truth
        # (Tomba2Engine/docs/findings/render.md) — it named SDK libgs builders as the producers.
        d.append(f"void {N.wrap}_{a:08X}(Core* c) {{ c->pc = {addr_const(a)}; "
                 f"if (c->pending_work) rec_irq_poll(c); c->idiag.otattrPush({addr_const(a)}); "
                 f"if ({N.ovtab}[{i}]) {{ {N.ovtab}[{i}](c); c->idiag.otattrPop(); return; }} "
                 f"{N.gen}_{a:08X}(c); c->idiag.otattrPop(); }}")
    d.append(f"int {N.index}(uint32_t addr) {{\n  switch (addr & 0x1FFFFFFFu) {{")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: return {idx[a]};")
    d.append("    default: return -1;\n  }\n}")
    d.append(f"void {N.setov}(uint32_t addr, OverrideFn fn) "
             f"{{ int i = {N.index}(addr); if (i >= 0) {N.ovtab}[i] = fn; }}")
    d.append(f"void {N.dispatch}(Core* c, uint32_t addr) {{\n  switch ({link_space('addr')} & 0x1FFFFFFFu) {{")
    for a in funcs:
        d.append(f"    case 0x{a & 0x1FFFFFFF:08X}u: {N.wrap}_{a:08X}(c); return;")
    d.append("    default: rec_dispatch_miss(c, addr); return;\n  }\n}")
    write_if_changed(os.path.join(out_dir, f"{N.disp}.c"), "\n".join(d) + "\n")
    print(f"[{N.wrap}] emitted {len(funcs)} functions -> {out_dir}/{N.shardpfx}_*.c "
          f"({shards} shards) + {N.decls}")
    # NATIVE-DEPTH COVERAGE, reported at BUILD time. At run time "0 vertices recorded" and "this game
    # has no 3D" are the same observation, so the number of taps the emitter actually placed has to be
    # visible here — a game whose submit idiom neither form matches shows up as 0 now, not as a mystery
    # later. `swc2` sites are inline (GTE_SCREEN_XY_REGS); `mfc2->sw` pairs come from vertex_pz_stores.
    global g_ds_borrowed, g_ds_unemittable
    # A dropped delay slot is a guest instruction that never runs, so this line prints EVERY run with
    # its denominators. `borrowed` is the fixed class (the slot lay outside the owning function).
    # `off-image` is the only remaining class that could still lose a real instruction, so it is
    # listed in full; `control`/`undecodable` slots are trailing DATA walked as code — a pre-existing
    # and expected condition in every image, so those are capped at a few examples rather than
    # dumping thousands of addresses over the interesting ones.
    by_cat = {}
    for a, cat in g_ds_unemittable:
        by_cat.setdefault(cat, []).append(a)
    def _sample(cat, cap):
        xs = sorted(set(by_cat.get(cat, ())))
        shown = xs if cap is None else xs[:cap]
        return (" ".join(f"0x{x:08X}" for x in shown)
                + (f" (+{len(xs) - len(shown)} more)" if len(shown) < len(xs) else "")) or "(none)"
    print(f"[{N.wrap}] delay slots: {g_ds_borrowed} borrowed from beyond the owning function's body "
          f"(a function entry starts ON a slot); not emitted: "
          f"{len(by_cat.get('off-image', ()))} off-image [{_sample('off-image', None)}], "
          f"{len(by_cat.get('control', ()))} control-in-slot [{_sample('control', 4)}], "
          f"{len(by_cat.get('undecodable', ()))} undecodable [{_sample('undecodable', 4)}] "
          f"— the last two are trailing data walked as code")
    g_ds_borrowed = 0; g_ds_unemittable = []
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
    check_seeds_in_text(exe, {address for checkpoint in gs["diagnostic_pcs"]
                              for address in checkpoint if address is not None},
                        "--seeds diagnostic_pcs owner/pc")
    ov_dir = sys.argv[sys.argv.index("--overlays") + 1] if "--overlays" in sys.argv else None
    seeds = ({exe.entry} | gs["main"] | gs["main_reentry"] | pointer_table_funcs(exe)
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
                            reentry=gs["main_reentry"], diagnostic_pcs=gs["diagnostic_pcs"])

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
    ov_images = []          # (stem, fn, base, data, ovexe, reloc_hi16 | None)
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
                              psexe.PsxExe(base, 0, base, len(data), 0, 0, data),
                              load_reloc_sidecar(os.path.join(ov_dir, fn[:-4] + ".reloc.json"), base,
                                                 len(data), fn)))
    # A0<n> sorts lexicographically in area order (A00..A09, A0A..A0L), which is the index order the
    # game's area byte uses; sorted(os.listdir) already put them that way.
    area_exes = [(stem, ovexe) for stem, _, _, _, ovexe, _ in ov_images
                 if re.fullmatch(r"A0[0-9A-Z]", stem)]
    area_tbl_seeds = area_indexed_overlay_tables(exe, area_exes)
    # Overlay -> overlay direct calls. A fixed-base image that calls into another co-resident image
    # leaves that entry undiscovered in the DESTINATION module (each module's jal scan is internal),
    # and demote_internal_labels then drops it entirely when the destination also branches there.
    # Relocatable modules have no static address, so they take no part.
    cross_ov_seeds = cross_overlay_call_targets(
        [(stem, ovexe) for stem, _, _, _, ovexe, hi16 in ov_images if hi16 is None])

    # ---- pass 2: emit each overlay module.
    overlays = []   # (tag, NAME, base, end, sig32 bytes, Names, relocatable)
    global g_mod
    for stem, fn, base, data, ovexe, hi16 in ov_images:
        tag = re.sub(r"[^a-z0-9]", "_", fn[:-4].lower())
        N = overlay_names(tag)
        # A module with a relocation sidecar is emitted BASE-RELATIVE (see ModuleReloc): its live
        # address is decided by the GAME's allocator at load time, so nothing here may bake it in.
        # The Core::ovDelta[] slot is the module's ordinal in g_rec_overlays[], which is this loop's
        # append order — the two must not drift, so the index is taken from the list being built.
        g_mod = None
        if hi16 is not None:
            if len(overlays) >= REC_MAX_OVERLAYS:
                sys.exit(f"[recomp] {fn} is relocatable module #{len(overlays)}, but Core::ovDelta[] "
                         f"holds {REC_MAX_OVERLAYS} (runtime/recomp/core.h kRecMaxOverlays). Raise it "
                         f"there and rebuild — a silently truncated table would mis-route dispatch.")
            g_mod = ModuleReloc(len(overlays), base, base + len(data), hi16)
            check_hi16_consumers(ovexe, hi16, fn)
        # Both the explicit seeds and the discovered per-area table entries are runtime-computed
        # ENTRY POINTS, so they get the same re-entry treatment (a preceding body that runs off its
        # end into one of them must continue into it, not `return`).
        explicit = OVERLAY_EXTRA_SEEDS.get(stem, set()) | area_tbl_seeds.get(stem, set())
        if not explicit and not looks_like_code(ovexe):
            # DATA through a code slot (a stream read into the overlay arena — see looks_like_code).
            # Emit an EMPTY module: the dispatch/index symbols must still exist (overlay_table.c
            # references them), the router keeps identifying the resident image by signature, and a
            # call into it falls to rec_dispatch_miss and fails fast. Recompiling it is never an
            # option — that is how 480KB of Spyro data became ~144MB of garbage C.
            unk, ret, prol = code_image_stats(ovexe)
            print(f"[recomp] overlay {fn}: NOT CODE ({unk:.0%} undecodable, {ret} jr-ra, "
                  f"{prol} prologues in {len(data)} bytes) — emitting an EMPTY module; the router "
                  f"keeps its signature entry and a call into it fails fast at dispatch")
            src_files += emit_module(ovexe, out_dir, N, set(), None, None, shards=2)
            overlays.append((tag, fn[:-4].upper(), base, base + len(data), data[:32], N, hi16 is not None))
            g_mod = None
            continue
        if explicit and not looks_like_code(ovexe):
            # Noreturn code (a stage main that never `jr ra`s — Tomba!2 START.BIN) trips the data
            # test; the game's explicit seeds vouch for it. Say so, so the override is auditable.
            print(f"[recomp] overlay {fn}: no jr-ra in image, but {len(explicit)} explicit seed(s) "
                  f"vouch for code — recompiling")
        # Cross-overlay call targets are real function ENTRIES (a direct `jal` names them), not
        # mid-function re-entry points, so they join `hard_ov` and stay out of `reentry`.
        hard_ov = (pointer_table_funcs(ovexe) | constructed_func_pointers(ovexe)
                   | code_pointer_tables(ovexe) | overlay_internal_jal_targets(ovexe)
                   | cross_ov_seeds.get(stem, set()) | explicit)
        soft_ov = func_entries_after_return(ovexe)   # jr-ra boundaries (mergeable if false)
        src_files += emit_module(ovexe, out_dir, N, hard_ov, None, None, shards=2,
                                 soft_seeds=soft_ov, reentry=explicit)
        overlays.append((tag, fn[:-4].upper(), base, base + len(data), data[:32], N, hi16 is not None))
        g_mod = None

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


def cross_overlay_call_targets(images):
    """Direct-`jal` targets that leave one overlay image and land inside ANOTHER overlay's image.

    `overlay_internal_jal_targets` only sees calls that stay inside one module, so an overlay that
    calls a fixed entry in a DIFFERENT, co-resident overlay leaves that entry undiscovered in the
    destination module. The destination is then emitted without it — and worse, when the destination
    also *branches* to that address from a neighbouring body, `demote_internal_labels` proves it an
    internal label and drops the callable entry outright. Its rule 1 ("control arrives with no return
    address, so it cannot be a function") is sound only over the references it can see; a direct
    `jal` from another module is exactly the counter-example, and it is direct evidence rather than
    an inference. So these targets are returned as HARD seeds — they enter `keep` and survive
    demotion, while the neighbouring body keeps using the same address through the ordinary
    cross-function paths. Measured: Vagrant INITBTL `0x800FA4A0` calls BATTLE `0x800E6EAC`
    (`jr ra` + delay slot — a null handler), which BATTLE's `0x800E6EB4` also branches to four times.

    TWO IMAGES THAT OVERLAP CANNOT BOTH BE RESIDENT, and that is the whole difficulty: the overlays
    live in a handful of fixed slots, so several images span any given address and only one of them
    is the real destination. Vagrant's BATTLE and TITLE share base `0x80068800` and BOTH span
    `0x800E6EAC`. Two filters decide it, and both are proofs rather than thresholds:

      1. RANGE DISJOINTNESS. A call from A to an address inside B is only executable when A and B are
         resident together, which requires their images not to overlap. This alone rejects every
         same-slot sibling as a *source*, but not as a *destination* candidate.
      2. THE DESTINATION MUST LOOK LIKE AN ENTRY THERE — `is_func_entry` (own prologue, or preceded
         by `jr ra` + delay slot), plus the bare `jr ra` null handler, the same test
         `area_indexed_overlay_tables` uses to reject a stale table slot. A slot sibling holds
         DIFFERENT code at the same address, so it fails this test except by coincidence.

    Measured on Vagrant, and run against both classes rather than reasoned about: of INITBTL's 26
    cross-image call targets, 26/26 pass the entry test in BATTLE and 0/26 pass it in TITLE. In the
    other direction BATTLE calls eight addresses in the `0x800F9800` slot that INITBTL spans, and
    0/8 pass there — correctly, because those calls belong to MAINMENU/SCREFF2, the other images
    that load into that slot and are not provisioned here.

    A RELOCATABLE module (one with a reloc sidecar) has no static address at all — its live base is
    chosen by the game's allocator — so it is neither a source nor a destination here; a caller
    reaches it through the router's per-Core delta instead.

    `images` is [(stem, PsxExe)] for the FIXED-BASE overlay images. Returns {stem: {addr, ...}}.
    Resident MAIN is deliberately not a source: a MAIN->overlay absolute call has its own declared
    mechanism (`overlay_seeds` in the game's seed file, plus `area_indexed_overlay_tables`), and
    MAIN is never ambiguous about which image is resident.
    """
    out = {stem: set() for stem, _ in images}
    if len(images) < 2:
        return out

    def overlaps(a, b):
        return a.load < b.text_end and b.load < a.text_end

    def callable_entry(ex, w):
        # A word that is the DELAY SLOT of the preceding control instruction can never be a separate
        # entry — that word executes as part of the branch's own flow (the same structurally
        # impossible class overlay_funcs rejects for resident targets).
        if w - 4 >= ex.load and decode(w - 4, ex.word(w - 4)).kind in (D.BRANCH, D.JUMP, D.JUMPR):
            return False
        return is_func_entry(ex, w) or ex.word(w) == 0x03E00008

    sites = 0            # cross-image call sites seen
    rejected_slot = 0    # destination candidate overlaps the source: they can never be co-resident
    rejected_entry = 0   # destination candidate is co-resident but holds no callable entry there
    for src_stem, sx in images:
        for a in range(sx.load, sx.text_end, 4):
            i = decode(a, sx.word(a))
            if i.op != "jal" or i.target is None or sx.load <= i.target < sx.text_end:
                continue
            hit = False
            for dst_stem, dx in images:
                if dst_stem == src_stem or not (dx.load <= i.target < dx.text_end):
                    continue
                hit = True
                if overlaps(sx, dx):
                    rejected_slot += 1
                elif callable_entry(dx, i.target):
                    out[dst_stem].add(i.target)
                else:
                    rejected_entry += 1
            sites += hit
    # PRINTED WITH ITS DENOMINATORS, ALWAYS. "0 seeded" is the right answer for a game whose overlays
    # never call each other, and it has to be distinguishable from "the scan never ran" and from "it
    # ran and rejected everything".
    seeded = sum(len(v) for v in out.values())
    print(f"[overlays] cross-overlay call scan: {sites} call site(s) into another image -> "
          f"{seeded} hard seed(s) in {sum(1 for v in out.values() if v)} destination module(s); "
          f"rejected {rejected_slot} same-slot candidate(s) (never co-resident) and "
          f"{rejected_entry} co-resident candidate(s) with no callable entry there"
          + (" ["
             + "; ".join(f"{stem}: " + " ".join(f"0x{x:08X}" for x in sorted(v))
                         for stem, v in sorted(out.items()) if v)
             + "]" if seeded else ""))
    return out


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
    """Emit generated/overlay_table.{h,c}: MAIN text range + per-overlay descriptor for the runtime
    router (overlay_router.cpp).

    `base`/`end` are the LINK range — where this module's code was recompiled to sit. For a
    RELOCATABLE module that is not where it runs: the game's allocator picks the live base at load
    time and the router adds the per-Core delta. For a fixed-base overlay the two coincide."""
    mlo, mhi = main_exe.load & 0x1FFFFFFF, main_exe.text_end & 0x1FFFFFFF
    h = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", "#pragma once",
         '#include "core.h"',
         '#include "recomp_iface.h"   // framework: struct RecOverlay (owned framework-side, not redefined here)', "",
         f"#define REC_MAIN_LO 0x{mlo:08X}u", f"#define REC_MAIN_HI 0x{mhi:08X}u", "",
         "void main_dispatch(Core*, uint32_t);", ""]
    for tag, NAME, base, end, sig, N, relocatable in overlays:
        h.append(f"void {N.dispatch}(Core*, uint32_t);")
        h.append(f"int {N.index}(uint32_t);")
    h += ["extern const RecOverlay g_rec_overlays[];", "extern const int g_rec_overlay_count;", ""]
    write_if_changed(os.path.join(out_dir, "overlay_table.h"), "\n".join(h) + "\n")

    c = ["// GENERATED by tools/recomp/emit.py — DO NOT EDIT.", '#include "overlay_table.h"', ""]
    for tag, NAME, base, end, sig, N, relocatable in overlays:
        bytes_c = ", ".join(f"0x{b:02X}" for b in sig)
        c.append(f"static const unsigned char sig_{tag}[{len(sig)}] = {{ {bytes_c} }};")
    c.append("")
    c.append("const RecOverlay g_rec_overlays[] = {")
    for tag, NAME, base, end, sig, N, relocatable in overlays:
        c.append(f"  {{ 0x{base:08X}u, 0x{end:08X}u, \"{NAME}\", {N.dispatch}, {N.index}, "
                 f"sig_{tag}, {len(sig)}, {1 if relocatable else 0} }},")
    c.append("};")
    c.append(f"const int g_rec_overlay_count = {len(overlays)};")
    write_if_changed(os.path.join(out_dir, "overlay_table.c"), "\n".join(c) + "\n")
    print(f"[overlays] {len(overlays)} overlay module(s) -> overlay_table.c")


if __name__ == "__main__":
    main()
