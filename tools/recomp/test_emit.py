"""Test-first suite for the RECOMPILER (tools/recomp/emit.py).

Two layers:
  1. STRUCTURAL tests — assert the static analyses (find_jump_tables both idiom variants, is_func_entry,
     code_pointer_tables / switch-table exclusion) on hand-assembled MIPS, with NO compilation. Fast; pin
     the exact transforms this session added.
  2. EXECUTION tests — assemble a tiny MIPS function, run it through emit_func -> C, compile that C against
     a minimal Core, EXECUTE it on concrete register/memory inputs, and assert the resulting state. This
     is differential TDD for the emitter: the recompiled body is the unit under test, its observable
     register/RAM effect is the assertion. Covers the tricky control flow (recovered jump table, shared
     epilogue register restore, branch-into-delay-slot, tail-call dispatch, tail-jump loop in O(1) stack)
     by building functions that exercise each and checking they compute the right answer. Skipped (not
     failed) if there is no C++ compiler.

Run: python3 tools/recomp/test_emit.py   (or: python3 -m pytest tools/recomp/test_emit.py -q)
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import decode as D
from decode import decode
import psexe
import emit

# ----------------------------------------------------------------------------------------------------
# Tiny MIPS assembler — just enough to write readable test functions. Two passes (resolve labels).
# ----------------------------------------------------------------------------------------------------
REG = {"zero": 0, "at": 1, "v0": 2, "v1": 3, "a0": 4, "a1": 5, "a2": 6, "a3": 7,
       "t0": 8, "t1": 9, "t2": 10, "t3": 11, "t4": 12, "t5": 13, "t6": 14, "t7": 15,
       "s0": 16, "s1": 17, "s2": 18, "s3": 19, "s4": 20, "s5": 21, "s6": 22, "s7": 23,
       "t8": 24, "t9": 25, "k0": 26, "k1": 27, "gp": 28, "sp": 29, "fp": 30, "ra": 31}
for _i in range(32):
    REG[f"r{_i}"] = _i


def _r(x):
    return REG[x] if isinstance(x, str) else x


def _rtype(rs, rt, rd, sh, fn):
    return (_r(rs) << 21) | (_r(rt) << 16) | (_r(rd) << 11) | (sh << 6) | fn


def _itype(op, rs, rt, imm):
    return (op << 26) | (_r(rs) << 21) | (_r(rt) << 16) | (imm & 0xFFFF)


class Asm:
    """Assemble a list of (mnemonic, *args) at `base`. Labels: a string arg to a branch/jump, or a
    standalone ('label', name). Returns bytes. Branch imm computed from final addresses."""
    def __init__(self, base=0x80010000):
        self.base = base
        self.items = []     # (kind, payload)
        self.labels = {}

    def _emit(self, mn, *args):
        self.items.append(("ins", (mn, args)))
        return self

    def label(self, name):
        self.items.append(("label", name))
        return self

    def __getattr__(self, mn):
        return lambda *args: self._emit(mn, *args)

    def assemble(self):
        # pass 1: addresses
        addr = self.base
        flat = []
        for kind, p in self.items:
            if kind == "label":
                self.labels[p] = addr
            else:
                flat.append((addr, p))
                addr += 4
        end = addr
        words = []
        for a, (mn, args) in flat:
            words.append(self._enc(a, mn, args))
        return struct.pack(f"<{len(words)}I", *words), end

    def _lbl(self, a, x):
        return self.labels[x] if isinstance(x, str) else x

    def _enc(self, a, mn, args):
        R = _rtype
        I = _itype
        if mn == "nop":
            return 0
        if mn in ("addu", "subu", "and_", "or_", "xor_", "nor", "slt", "sltu", "add", "sub"):
            fn = {"add": 0x20, "addu": 0x21, "sub": 0x22, "subu": 0x23, "and_": 0x24, "or_": 0x25,
                  "xor_": 0x26, "nor": 0x27, "slt": 0x2A, "sltu": 0x2B}[mn]
            rd, rs, rt = args
            return R(rs, rt, rd, 0, fn)
        if mn in ("sll", "srl", "sra"):
            fn = {"sll": 0, "srl": 2, "sra": 3}[mn]
            rd, rt, sh = args
            return R(0, rt, rd, sh, fn)
        if mn in ("sllv", "srlv", "srav"):
            fn = {"sllv": 4, "srlv": 6, "srav": 7}[mn]
            rd, rt, rs = args
            return R(rs, rt, rd, 0, fn)
        if mn in ("mult", "multu", "div", "divu"):
            fn = {"mult": 0x18, "multu": 0x19, "div": 0x1A, "divu": 0x1B}[mn]
            rs, rt = args
            return R(rs, rt, 0, 0, fn)
        if mn == "mfhi":
            return R(0, 0, args[0], 0, 0x10)
        if mn == "mflo":
            return R(0, 0, args[0], 0, 0x12)
        if mn == "jr":
            return R(args[0], 0, 0, 0, 8)
        if mn == "jalr":
            return R(args[0], 0, 31, 0, 9)
        if mn in ("addiu", "addi", "andi", "ori", "xori", "slti", "sltiu"):
            op = {"addi": 8, "addiu": 9, "slti": 0xA, "sltiu": 0xB, "andi": 0xC, "ori": 0xD, "xori": 0xE}[mn]
            rt, rs, imm = args
            return I(op, rs, rt, imm)
        if mn == "lui":
            return I(0xF, 0, args[0], args[1])
        if mn == "mfc2":                    # cop2 move-from: op 0x12, fmt 0, rt = GPR, rd = cop2 data reg
            rt, rd = args
            return (0x12 << 26) | (0 << 21) | (_r(rt) << 16) | (rd << 11)
        if mn == "rtps":                    # a COP2 op (bit25 set) — moves the GTE FIFOs
            return (0x12 << 26) | (1 << 25) | 0x01
        if mn in ("swc2", "lwc2"):          # COP2 store/load: op 0x3A/0x32, rt = cop2 data reg
            op = 0x3A if mn == "swc2" else 0x32
            rt, off, rs = args
            return I(op, rs, rt, off)
        if mn in ("lw", "lh", "lhu", "lb", "lbu", "sw", "sh", "sb"):
            op = {"lb": 0x20, "lh": 0x21, "lw": 0x23, "lbu": 0x24, "lhu": 0x25,
                  "sb": 0x28, "sh": 0x29, "sw": 0x2B}[mn]
            rt, off, rs = args
            return I(op, rs, rt, off)
        if mn in ("beq", "bne"):
            op = {"beq": 4, "bne": 5}[mn]
            rs, rt, tgt = args
            imm = (self._lbl(a, tgt) - (a + 4)) >> 2
            return I(op, rs, rt, imm)
        if mn in ("blez", "bgtz"):
            op = {"blez": 6, "bgtz": 7}[mn]
            rs, tgt = args
            imm = (self._lbl(a, tgt) - (a + 4)) >> 2
            return I(op, rs, 0, imm)
        if mn in ("bltz", "bgez", "bltzal", "bgezal"):
            # REGIMM: the LINKING forms are rt 0x10/0x11. They are calls wearing a branch's
            # encoding, and the demotion criterion has to tell them apart from plain branches.
            rt = {"bltz": 0, "bgez": 1, "bltzal": 0x10, "bgezal": 0x11}[mn]
            rs, tgt = args
            imm = (self._lbl(a, tgt) - (a + 4)) >> 2
            return I(1, rs, rt, imm)
        if mn == "b":     # beq zero,zero
            imm = (self._lbl(a, args[0]) - (a + 4)) >> 2
            return I(4, 0, 0, imm)
        if mn in ("j", "jal"):
            op = {"j": 2, "jal": 3}[mn]
            tgt = self._lbl(a, args[0])
            return (op << 26) | ((tgt >> 2) & 0x3FFFFFF)
        raise ValueError(f"asm: unknown {mn}")


def exe_of(data, base=0x80010000):
    return psexe.PsxExe(base, 0, base, len(data), 0, 0, data)


# ----------------------------------------------------------------------------------------------------
# 1. STRUCTURAL TESTS
# ----------------------------------------------------------------------------------------------------
def _emit_checkpoint_fixture(out_dir, diagnostic_pcs=None):
    a = Asm()
    a.addiu("v0", "zero", 1)
    a.addiu("v1", "zero", 2)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    kwargs = {} if diagnostic_pcs is None else {"diagnostic_pcs": set(diagnostic_pcs)}
    emit.emit_module(exe_of(data), out_dir, emit.MAIN_NAMES, {a.base}, shards=1, **kwargs)
    return {name: open(os.path.join(out_dir, name), "rb").read()
            for name in sorted(os.listdir(out_dir))}


def test_diagnostic_checkpoint_empty_set_is_byte_identical_to_legacy_output():
    with tempfile.TemporaryDirectory() as legacy, tempfile.TemporaryDirectory() as explicit:
        assert _emit_checkpoint_fixture(legacy) == _emit_checkpoint_fixture(explicit, set())


def test_diagnostic_checkpoints_precede_two_selected_ordinary_instructions():
    with tempfile.TemporaryDirectory() as td:
        files = _emit_checkpoint_fixture(td, {0x80010000, 0x80010004})
        shard = files[emit.MAIN_NAMES.shardpfx + "_0.c"].decode()
        assert shard.count("pc_observer_at(c,") == 2
        assert "pc_observer_at(c, 0x80010000u);\n  c->r[2] =" in shard
        assert "pc_observer_at(c, 0x80010004u);\n  c->r[3] =" in shard


def test_diagnostic_checkpoint_outside_emitted_text_refuses_with_denominator():
    with tempfile.TemporaryDirectory() as td:
        try:
            _emit_checkpoint_fixture(td, {0x80010100})
        except SystemExit as error:
            message = str(error)
            assert "requested=1 emitted=0" in message
            assert "0x80010100" in message
        else:
            assert False, "an unemitted requested checkpoint must refuse generation"


def test_jumptable_idiom_A():
    # Variant A: lui base,HI ; addiu base,base,LO ; addu base,base,idx ; lw rN,0(base) ; jr rN
    # Table at 0x80010400 with 3 entries inside the function.
    a = Asm(0x80010000)
    a.sltiu("v0", "a0", 3)
    a.beq("v0", "zero", "deflt")
    a.sll("v1", "a0", 2)            # idx*4 (delay-ish, but standalone here is fine for table recovery)
    a.lui("v0", 0x8001)
    a.addiu("v0", "v0", 0x0400)     # table base 0x80010400
    a.addu("v0", "v0", "v1")
    a.lw("v0", 0, "v0")
    a.jr("v0")
    a.nop()
    a.label("deflt")
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    # 3-entry table at offset 0x400 pointing at in-function case labels (validate keeps targets in-range)
    tgts = [0x80010020, 0x80010024, 0x80010028]
    blob = bytearray(0x400 + 12)
    blob[0:len(data)] = data
    struct.pack_into("<3I", blob, 0x400, *tgts)
    e = exe_of(bytes(blob))
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.load + len(data), 4)}
    jt = emit.find_jump_tables(e, ins, e.load, e.load + len(data))
    jr_addr = 0x80010000 + 7 * 4
    assert jr_addr in jt, f"variant-A table not recovered: {jt}"
    assert jt[jr_addr] == tgts, jt[jr_addr]


def test_jumptable_idiom_B():
    # Variant B (the overlay form): lui tbl,HI ; addiu tbl,tbl,LO ; addu B,idx,tbl ; lw rN,0(B) ; jr rN
    # — table address built in a SEPARATE reg (v0) and added to the index reg (v1, which is the lw base).
    a = Asm(0x80010000)
    a.sltiu("v0", "a0", 2)          # bounds check -> the table COUNT (=2)
    a.beq("v0", "zero", "deflt")
    a.sll("v1", "a0", 2)            # v1 = idx*4 (the lw base reg)
    a.lui("v0", 0x8001)            # v0 = table HI
    a.addiu("v0", "v0", 0x0400)    # v0 = 0x80010400
    a.addu("v1", "v1", "v0")       # v1 = idx*4 + table   (base reg defined via addu of the OTHER reg)
    a.lw("v0", 0, "v1")
    a.jr("v0")
    a.nop()
    a.label("deflt")
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    tgts = [0x80010020, 0x80010024]
    blob = bytearray(0x400 + 8)
    blob[0:len(data)] = data
    struct.pack_into("<2I", blob, 0x400, *tgts)
    e = exe_of(bytes(blob))
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.load + len(data), 4)}
    jt = emit.find_jump_tables(e, ins, e.load, e.load + len(data))
    jr_addr = 0x80010000 + 7 * 4
    assert jr_addr in jt, f"variant-B (addu) table not recovered: {jt}"
    assert jt[jr_addr] == tgts, jt[jr_addr]


def test_jumptable_base_built_in_separate_reg():
    # Regression (later-272): the table BASE is built in one reg via `lui tmp,HI ; addiu base,tmp,LO`
    # where the addiu's src != dst (base is a SEPARATE reg from the lui target), then `addu jr, idx, base`.
    # This is FUN_8003c048's entity-handler dispatcher (`lui v0,0x8001 ; addiu s3,v0,0x4db8 ; ... ;
    # addu v0,idx,s3 ; lw v0,0(v0) ; jr v0`, table 0x80014db8). Pre-fix find_jump_tables captured the HI
    # half but DROPPED the LO (it required addiu rs==rt) -> wrong table addr -> recovery failed -> the
    # in-function `jr` fell back to `rec_dispatch(...);return;`, skipping the epilogue (callee-saved + SP
    # restore) and corrupting the caller (the GAME-loop freeze). Must recover the table so a C `switch`
    # keeps the `jr` INSIDE the function.
    # Faithful to FUN_8003c048: table base in callee-saved s3 built at entry via `lui t0; addiu s3,t0,LO`
    # (rs!=rt), and the index reg v0 is REUSED both as the `lw` base / `jr` reg AND by an intervening
    # `lui v0` scratch between the table setup and the jr. The fix must (a) follow `addiu s3,src,LO` to its
    # `lui src` even though src!=s3, and (b) drop the scaled-index reg v0 so the intervening `lui v0` trap
    # isn't matched as the table HI.
    a = Asm(0x80010000)
    a.lui("t0", 0x8001)            # t0 = table HI
    a.addiu("s3", "t0", 0x0400)    # s3 = 0x80010400  (rs=t0 != rt=s3 — the separate-base form)
    a.sltiu("v0", "a0", 3)          # bounds check -> COUNT (=3)
    a.beq("v0", "zero", "deflt")
    a.lui("v0", 0x1f80)            # TRAP: scratch lui to v0 (would be matched as HI if v0 not excluded)
    a.sll("v0", "a0", 2)            # v0 = idx*4   (index reg == lw base == jr reg, like FUN_8003c048)
    a.addu("v0", "v0", "s3")       # v0 = idx*4 + table base
    a.lw("v0", 0, "v0")
    a.jr("v0")
    a.nop()
    a.label("deflt")
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    tgts = [0x80010020, 0x80010024, 0x80010028]
    blob = bytearray(0x400 + 12)
    blob[0:len(data)] = data
    struct.pack_into("<3I", blob, 0x400, *tgts)
    e = exe_of(bytes(blob))
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.load + len(data), 4)}
    jt = emit.find_jump_tables(e, ins, e.load, e.load + len(data))
    jr_addr = 0x80010000 + 8 * 4   # lui,addiu,sltiu,beq,lui(trap),sll,addu,lw,JR -> index 8
    assert jr_addr in jt, f"separate-base table not recovered (rs!=rt addiu / index-reg reuse): {jt}"
    assert jt[jr_addr] == tgts, jt[jr_addr]


def test_prologue_soft_seed_survives_merge():
    # Regression (later-272): a soft boundary (func_entries_after_return, preceded by `jr ra`) that
    # allocates its OWN stack frame (`addiu sp,sp,-N` prologue) is a GENUINE function — even when an
    # earlier function has a forward branch crossing it (the shared-tail/early-return pattern the merge
    # normally collapses). It must NOT be merged: an overlay handler reached ONLY via a runtime
    # object-method pointer is invisible to static jal discovery, so merging it makes that computed call
    # fail-fast (recomp-MISS 0x80146478, the narration-cutscene actor's `addiu sp,-0x20` update method).
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -0x20)      # 0x00  H: a real function (hard entry)
    a.beq("a0", "zero", "cross")    # 0x04  forward branch that crosses g into [g, next)
    a.nop()                          # 0x08
    a.addu("v0", "v0", "v1")        # 0x0C  H body
    a.nop()                          # 0x10
    a.nop()                          # 0x14
    a.jr("ra")                       # 0x18  (so 0x20 is a func_entries_after_return soft seed)
    a.nop()                          # 0x1C  delay
    a.addiu("sp", "sp", -0x10)      # 0x20  g: a REAL function — its OWN prologue; soft seed
    a.nop()                          # 0x24
    a.label("cross")
    a.addu("v0", "a0", "a1")        # 0x28  branch target (lands inside g's range)
    a.jr("ra")                       # 0x2C
    a.nop()                          # 0x30
    data, end = a.assemble()
    e = exe_of(data)
    H, g = 0x80010000, 0x80010020
    has_prologue = {f for f in (g,) if (e.word(f) & 0xFFFF8000) == 0x27BD8000}
    assert g in has_prologue, "g must be recognized as a stack-prologue function"
    # NEW: prologue g excluded from `removable` -> merge KEEPS it.
    kept = emit.merge_early_return_boundaries(e, [H, g], {g} - has_prologue, {H})
    assert g in kept, f"prologue soft-seed wrongly merged: {[hex(x) for x in kept]}"
    # The branch genuinely crosses g, so the OLD behavior (g removable) WOULD merge it — confirms the
    # test exercises the merge path, i.e. the prologue guard is what saves g.
    merged = emit.merge_early_return_boundaries(e, [H, g], {g}, {H})
    assert g not in merged, "expected the branch-cross to merge g when it is removable"





def test_is_func_entry():
    # (a) addiu sp,sp,-N prologue ; (b) preceded by `jr ra; <delay>`.
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -32)       # 0x80010000: prologue -> entry by signal (a)
    a.jr("ra")                     # 0x80010004
    a.nop()                        # 0x80010008
    a.lui("v0", 0x8001)            # 0x8001000C: starts right after jr-ra+delay -> entry by signal (b)
    data, end = a.assemble()
    e = exe_of(data)
    assert emit.is_func_entry(e, 0x80010000)
    assert emit.is_func_entry(e, 0x8001000C)
    assert not emit.is_func_entry(e, 0x80010004)   # the `jr ra` itself is not an entry


def test_switch_table_excluded_from_code_pointer_tables():
    # A recovered switch jump-table's case-label array must NOT be mistaken for a vtable and seeded.
    a = Asm(0x80010000)
    a.sltiu("v0", "a0", 2)
    a.beq("v0", "zero", "deflt")
    a.sll("v1", "a0", 2)
    a.lui("v0", 0x8001)
    a.addiu("v0", "v0", 0x0040)
    a.addu("v0", "v0", "v1")
    a.lw("v0", 0, "v0")
    a.jr("v0")
    a.nop()
    a.label("deflt")
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    buf = bytearray(0x48)
    buf[0:len(data)] = data
    # the 2-entry switch table at 0x80010040 points at in-function case labels
    struct.pack_into("<2I", buf, 0x40, 0x80010028, 0x8001002C)
    e = exe_of(bytes(buf))
    spans = emit.switch_table_spans(e)
    assert 0x80010040 in spans, "switch table data span not detected"
    cpt = emit.code_pointer_tables(e)
    assert 0x80010028 not in cpt and 0x8001002C not in cpt, \
        "switch case-labels wrongly seeded as a code-pointer table (vtable)"


# ----------------------------------------------------------------------------------------------------
# 2. EXECUTION TESTS — compile the emitted C against a minimal Core, run, assert register/RAM state.
# ----------------------------------------------------------------------------------------------------
HARNESS = r"""
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
static void rec_irq_poll(struct Core*);
struct InterpDiagFixture {
  uint32_t otattr_pushes = 0, otattr_pops = 0, otattr_depth = 0;
  void otattrPush(uint32_t){ ++otattr_pushes; ++otattr_depth; }
  void otattrPop(){ if (!otattr_depth) std::abort(); ++otattr_pops; --otattr_depth; }
};
struct Core {
  uint32_t r[32]; uint32_t lo, hi, pc;
  uint32_t pending_work = 0;      // upstream's deferred-work gate tests this at entries/back-edges
  InterpDiagFixture idiag;        // generated wrappers maintain the shipping OT-attribution stack
  uint8_t ram[0x200000];
  uint8_t  mem_r8 (uint32_t a){ return ram[a & 0x1FFFFF]; }
  uint16_t mem_r16(uint32_t a){ uint16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }
  uint32_t mem_r32(uint32_t a){ uint32_t v; memcpy(&v, ram + (a & 0x1FFFFF), 4); return v; }
  void mem_w8 (uint32_t a, uint8_t v){ ram[a & 0x1FFFFF] = v; }
  void mem_w16(uint32_t a, uint16_t v){ memcpy(ram + (a & 0x1FFFFF), &v, 2); }
  void mem_w32(uint32_t a, uint32_t v){ memcpy(ram + (a & 0x1FFFFF), &v, 4); }
  uint32_t mem_lwl(uint32_t cur, uint32_t a){ uint32_t al=a&~3u,sh=(a&3)*8; uint32_t w=mem_r32(al);
    uint32_t m = sh==24?0xFFFFFFFFu:((1u<<(8+sh))-1)<<(24-sh); /*unused in tests*/ (void)m; (void)cur;
    return (cur & ((1u<<(24-sh))-1)) | (w << (24-sh)); }
  uint32_t mem_lwr(uint32_t cur, uint32_t a){ uint32_t al=a&~3u,sh=(a&3)*8; uint32_t w=mem_r32(al);
    (void)cur; return (sh==0)?w:((cur & ~((0xFFFFFFFFu)>>sh)) | (w >> sh)); }
};
static void cpu_div(Core* c, uint32_t a, uint32_t b){ int32_t x=(int32_t)a,y=(int32_t)b;
  if(!y){ c->lo=(x>=0)?0xFFFFFFFFu:1u; c->hi=(uint32_t)x; }
  else if((uint32_t)x==0x80000000u && y==-1){ c->lo=0x80000000u; c->hi=0; }
  else { c->lo=(uint32_t)(x/y); c->hi=(uint32_t)(x%y); } }
static void cpu_divu(Core* c, uint32_t a, uint32_t b){ if(!b){ c->lo=0xFFFFFFFFu; c->hi=a; }
  else { c->lo=a/b; c->hi=a%b; } }
// Native-depth taps. The emitter now places these in ordinary functions (any lw->sw word copy), so
// the execution harness has to satisfy them. They have no effect on the register/RAM assertions.
void gte_hold_pz(Core*, int, int) {}
void gte_record_pz(Core*, uint32_t, int) {}
void gte_hold_src(Core*, int, uint32_t) {}
void gte_copy_pz(Core*, int, uint32_t) {}
uint32_t gte_read_data(uint32_t){ return 0; }
static void rec_irq_poll(Core*) {}
uint32_t g_dispatch = 0;
void (*g_dispatch_fn)(Core*) = 0;   // when set, rec_dispatch TAIL-calls it (models the loop back-edge)
void rec_dispatch(Core* c, uint32_t addr){ g_dispatch = addr; if (g_dispatch_fn) g_dispatch_fn(c); }
__HOOKS__
__BODY__
int main(int argc, char** argv){
  static Core c; memset(&c, 0, sizeof(c));
  for(int i=1;i<argc;i++){ unsigned idx,val; if(sscanf(argv[i],"r%u=%x",&idx,&val)==2) c.r[idx]=val;
    else { unsigned ad; if(sscanf(argv[i],"m%x=%x",&ad,&val)==2) c.mem_w32(ad,val); } }
  __PRECALL__
  __ENTRY__(&c);
  for(int i=0;i<32;i++) printf("r%d=%08x\n", i, c.r[i]);
  printf("lo=%08x\nhi=%08x\ndispatch=%08x\n", c.lo, c.hi, g_dispatch);
  return 0;
}
"""


def _have_cxx():
    for cc in ("c++", "g++", "clang++"):
        try:
            subprocess.run([cc, "--version"], capture_output=True, check=True)
            return cc
        except Exception:
            continue
    return None


def run_func(data, base, regs=None, mem=None, hooks="", base_exe=0x80010000, funcset=None, precall="",
             hi=None):
    """emit_func(base) -> C, compile with the harness, run with regs/mem, return {'r':[...],'lo','hi',
    'dispatch'}. `hooks` = extra C (e.g. stub func_<addr> tail-call targets); `precall` = C run in main
    just before the entry call (e.g. set g_dispatch_fn for a self-dispatch loop); `hi` overrides the
    function end (so a branch target PAST hi exercises out-of-[lo,hi) handling)."""
    cc = _have_cxx()
    if not cc:
        return None
    e = exe_of(data, base_exe)
    funcset = funcset or {base}
    out = []
    emit.emit_func(e, base, hi if hi is not None else e.text_end, set(funcset), out,
                   f"gen_func_{base:08X}", emit.MAIN_NAMES)
    body = "\n".join(out)
    src = (HARNESS.replace("__BODY__", body)
                  .replace("__ENTRY__", f"gen_func_{base:08X}")
                  .replace("__PRECALL__", precall)
                  .replace("__HOOKS__", hooks))
    with tempfile.TemporaryDirectory() as td:
        cpp = os.path.join(td, "t.cpp")
        binp = os.path.join(td, "t")
        open(cpp, "w").write(src)
        # Match the REAL build (cmake): the generated tail dispatches need sibling-call optimization or
        # a guest tail-jump LOOP grows the C stack -> overflow. -O2 enables it; pin it here too.
        r = subprocess.run([cc, "-O2", "-foptimize-sibling-calls", "-w", "-o", binp, cpp],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"harness compile failed:\n{r.stderr}\n--- src ---\n{src}"
        args = [binp]
        for k, v in (regs or {}).items():
            args.append(f"r{_r(k)}={v & 0xFFFFFFFF:x}")
        for ad, v in (mem or {}).items():
            args.append(f"m{ad:x}={v & 0xFFFFFFFF:x}")
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    res = {"r": [0] * 32}
    for line in out.splitlines():
        key, _, val = line.partition("=")
        v = int(val, 16)
        if key.startswith("r"):
            res["r"][int(key[1:])] = v
        else:
            res[key] = v
    return res


def run_module(data, base, entry, regs=None, base_exe=0x80010000, seeds=None):
    """WHOLE-PIPELINE execution test: emit_module(exe) -> shards + dispatch TU, compile them together
    with the harness, run `entry`, return the same dict as run_func.

    run_func calls emit_func with a funcset the TEST hands it, so it cannot see anything that
    emit_module decides — which function set discovery produces, and what emit_module does to it
    afterwards. RE-16 lived exactly there: `demote_internal_labels` was written, unit-tested and
    never wired in, and no emit_func-level test could have noticed. This one runs the wiring."""
    cc = _have_cxx()
    if not cc:
        return None
    e = exe_of(data, base_exe)
    with tempfile.TemporaryDirectory() as td:
        # core.h is what the generated TUs include; give them the harness's Core plus the runtime
        # symbols the dispatch TU references.
        prelude = HARNESS.split("__HOOKS__")[0]
        # Every TU includes this, so the harness's definitions have to be inline/static here.
        for sym in ("void gte_hold_pz", "void gte_record_pz", "void gte_hold_src", "void gte_copy_pz",
                    "uint32_t gte_read_data", "void rec_dispatch(", "uint32_t g_dispatch",
                    "void (*g_dispatch_fn)"):
            prelude = prelude.replace(sym, "inline " + sym)
        core_h = ("#pragma once\n" + prelude
                  + "\ntypedef void (*OverrideFn)(Core*);\n"
                    "inline void rec_dispatch_miss(Core* c, uint32_t a){ rec_dispatch(c, a); }\n")
        open(os.path.join(td, "core.h"), "w").write(core_h)
        srcs = emit.emit_module(e, td, emit.MAIN_NAMES, seeds or {base}, shards=1)
        main_cpp = os.path.join(td, "main.cpp")
        open(main_cpp, "w").write(
            '#include "rec_decls.h"\n#include <cstdio>\n#include <cstring>\n'
            "int main(int argc, char** argv){\n"
            "  static Core c; memset(&c, 0, sizeof(c));\n"
            "  for(int i=1;i<argc;i++){ unsigned idx,val; if(sscanf(argv[i],\"r%u=%x\",&idx,&val)==2) c.r[idx]=val;\n"
            "    else { unsigned ad; if(sscanf(argv[i],\"m%x=%x\",&ad,&val)==2) c.mem_w32(ad,val); } }\n"
            f"  gen_func_{entry:08X}(&c);\n"
            '  for(int i=0;i<32;i++) printf("r%d=%08x\\n", i, c.r[i]);\n'
            '  printf("lo=%08x\\nhi=%08x\\ndispatch=%08x\\n", c.lo, c.hi, g_dispatch);\n'
            '  printf("otpush=%08x\\notpop=%08x\\notdepth=%08x\\n", c.idiag.otattr_pushes, '
            'c.idiag.otattr_pops, c.idiag.otattr_depth);\n'
            "  return 0;\n}\n")
        binp = os.path.join(td, "t")
        cmd = [cc, "-O2", "-foptimize-sibling-calls", "-w", f"-I{td}", "-o", binp, "-x", "c++",
               main_cpp] + [os.path.join(td, s) for s in srcs]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, f"module compile failed:\n{r.stderr}"
        args = [binp] + [f"r{_r(k)}={v & 0xFFFFFFFF:x}" for k, v in (regs or {}).items()]
        out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    res = {"r": [0] * 32}
    for line in out.splitlines():
        key, _, val = line.partition("=")
        v = int(val, 16)
        if key.startswith("r"):
            res["r"][int(key[1:])] = v
        else:
            res[key] = v
    return res


def _skip_if_no_cc():
    if not _have_cxx():
        print("   (no C++ compiler — execution tests skipped)")
        return True
    return False


def test_exec_basic_alu():
    if _skip_if_no_cc():
        return
    a = Asm(0x80010000)
    a.addu("v0", "a0", "a1")       # v0 = a0 + a1
    a.sll("v0", "v0", 1)           # v0 <<= 1
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    res = run_func(data, 0x80010000, regs={"a0": 10, "a1": 5})
    assert res["r"][2] == (10 + 5) * 2, res["r"][2]


def test_exec_loop_sum():
    if _skip_if_no_cc():
        return
    # v0 = sum(1..a0)  via a backward branch (loop), exercising fall-through + branch-to-label.
    a = Asm(0x80010000)
    a.addiu("v0", "zero", 0)       # sum = 0
    a.label("top")
    a.addu("v0", "v0", "a0")       # sum += a0
    a.addiu("a0", "a0", -1)        # a0--
    a.bne("a0", "zero", "top")
    a.nop()
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    res = run_func(data, 0x80010000, regs={"a0": 5})
    assert res["r"][2] == 15, res["r"][2]


def test_exec_jumptable():
    if _skip_if_no_cc():
        return
    # switch(a0){0:v0=11; 1:v0=22; 2:v0=33;}  via a recovered jump table.
    a = Asm(0x80010000)
    a.sltiu("v0", "a0", 3)
    a.beq("v0", "zero", "deflt")
    a.sll("v1", "a0", 2)
    a.lui("v0", 0x8001)
    a.addiu("v0", "v0", 0x0100)    # table @ 0x80010100
    a.addu("v0", "v0", "v1")
    a.lw("v0", 0, "v0")
    a.jr("v0")
    a.nop()
    a.label("c0")
    a.addiu("v0", "zero", 11)
    a.jr("ra")
    a.nop()
    a.label("c1")
    a.addiu("v0", "zero", 22)
    a.jr("ra")
    a.nop()
    a.label("c2")
    a.addiu("v0", "zero", 33)
    a.jr("ra")
    a.nop()
    a.label("deflt")
    a.addiu("v0", "zero", 99)
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    buf = bytearray(0x10C)
    buf[0:len(data)] = data
    L = a.labels
    struct.pack_into("<3I", buf, 0x100, L["c0"], L["c1"], L["c2"])
    # The recovered switch keeps the `lw v0,0(v0)` (it switches on the loaded value), so the table data
    # must also live in Core RAM for the load to read the right case address.
    tbl_mem = {0x80010100: L["c0"], 0x80010104: L["c1"], 0x80010108: L["c2"]}
    for inp, want in ((0, 11), (1, 22), (2, 33), (5, 99)):
        res = run_func(bytes(buf), 0x80010000, regs={"a0": inp}, mem=dict(tbl_mem))
        assert res["r"][2] == want, f"switch({inp}) -> {res['r'][2]:#x} != {want}"


def test_exec_branch_into_delay_slot():
    if _skip_if_no_cc():
        return
    # Construct: bltz a0, TGT ; <fallthrough sets v0=1> ; b OVER ; TGT(=delay of an uncond b): v0=2 ...
    # Mirror the real 0x80084080 shape: an unconditional `b` whose DELAY SLOT is the target of a `bltz`.
    a = Asm(0x80010000)
    a.bltz("a0", "ds")             # if a0<0 -> jump INTO the delay slot below
    a.nop()
    a.addiu("v0", "zero", 1)       # a0>=0 path: v0=1
    a.b("done")                    # unconditional branch ...
    a.label("ds")
    a.addiu("v0", "zero", 2)       # ... whose DELAY SLOT is `v0=2` and is ALSO the bltz target
    a.label("done")
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    # a0 >= 0: falls through -> v0=1, then b's delay slot sets v0=2 -> done. So v0=2 either way? Check the
    # two paths give the MIPS-correct result: pos path runs v0=1 then b(delay v0=2)->done => v0=2;
    # neg path jumps to ds (v0=2) ->done => v0=2. Both 2 — but the POINT is the neg path must REACH ds.
    pos = run_func(data, 0x80010000, regs={"a0": 5})
    neg = run_func(data, 0x80010000, regs={"a0": 0xFFFFFFFF})
    assert pos["r"][2] == 2, pos["r"][2]
    assert neg["r"][2] == 2, neg["r"][2]


def test_exec_shared_epilogue_restores_regs():
    if _skip_if_no_cc():
        return
    # Two return paths share one epilogue that restores s0 from the stack — the early branch must reach
    # the shared epilogue (floodfill duplication), so s0 is restored on BOTH paths.
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -16)
    a.sw("s0", 8, "sp")            # save caller s0
    a.addiu("s0", "zero", 0x77)    # clobber s0
    a.bne("a0", "zero", "epi")     # early exit -> shared epilogue
    a.nop()
    a.addiu("v0", "zero", 1)       # a0==0 path
    a.label("epi")
    a.lw("s0", 8, "sp")            # RESTORE s0  (shared epilogue)
    a.addiu("sp", "sp", 16)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    # set sp to a scratch area and a known caller-s0; assert s0 restored regardless of a0.
    for a0 in (0, 1):
        res = run_func(data, 0x80010000, regs={"a0": a0, "sp": 0x801000, "s0": 0xCAFE})
        assert res["r"][16] == 0xCAFE, f"a0={a0}: s0={res['r'][16]:#x} not restored (epilogue missed)"


def test_exec_tail_call_dispatches():
    if _skip_if_no_cc():
        return
    # `j sibling` where sibling is NOT in funcset -> emit routes it through rec_dispatch (the harness
    # records the address). Confirms floodfill treats it as a tail call, not intra-flow.
    a = Asm(0x80010000)
    a.addiu("v0", "zero", 5)
    a.j(0x80055555 & ~3)           # tail call to an external address (not in funcset)
    a.nop()
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    res = run_func(data, 0x80010000, regs={})
    assert res["dispatch"] == (0x80055555 & ~3), f"tail call not dispatched: {res['dispatch']:#x}"


def test_exec_cross_function_shared_epilogue():
    # A branch to a SHARED EPILOGUE that lives PAST this function's `hi` (in a sibling's range) — the A00
    # 0x80113100 -> 0x80113328 shape. emit_func must DUPLICATE the out-of-[lo,hi) tail so the branch runs
    # it (restoring s0) instead of routing to the dispatcher (which would skip the restore). hi is set to
    # exclude the epilogue.
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -16)
    a.sw("s0", 8, "sp")            # save caller s0
    a.addiu("s0", "zero", 0x77)    # clobber s0
    a.bne("a0", "zero", "epi")     # branch to the shared epilogue (past hi)
    a.nop()
    a.addiu("v0", "zero", 1)       # a0==0 path
    a.b("epi")
    a.nop()
    hi_marker = len(a.items)       # epilogue starts here -> compute its address as hi
    a.label("epi")
    a.lw("s0", 8, "sp")            # RESTORE s0  (shared epilogue, conceptually owned by a sibling)
    a.addiu("sp", "sp", 16)
    a.jr("ra")
    a.nop()
    data, end = a.assemble()
    epi = a.labels["epi"]
    for a0 in (0, 1):
        res = run_func(data, 0x80010000, regs={"a0": a0, "sp": 0x801000, "s0": 0xCAFE}, hi=epi)
        assert res is not None
        assert res["r"][16] == 0xCAFE, \
            f"a0={a0}: s0={res['r'][16]:#x} not restored (out-of-range shared epilogue not duplicated)"


def test_exec_tail_jump_loop_is_O1_stack():
    if _skip_if_no_cc():
        return
    # A guest LOOP whose back-edge is a COMPUTED `jr t9` (register-held target -> not statically
    # recoverable -> emitted as `rec_dispatch(c, t9); return;`). The harness rec_dispatch tail-calls the
    # entry, modelling the loop. With sibling-call optimization the whole tail chain is O(1) stack and
    # the loop completes; WITHOUT it (the -O1 bug we just fixed) it grows the stack and SIGSEGVs. Running
    # 200k iterations to completion IS the assertion that tail dispatches don't grow the stack.
    a = Asm(0x80010000)
    a.blez("a0", "done")           # a0 = remaining count; <=0 -> return
    a.nop()
    a.addu("v0", "v0", "a0")       # v0 += a0
    a.addiu("a0", "a0", -1)        # a0--
    a.lw("t9", 0, "sp")            # t9 = loop-head address (register-based -> unrecoverable jr)
    a.jr("t9")                     # -> rec_dispatch(c, t9) -> (harness) gen_func entry  [TAIL]
    a.nop()
    a.label("done")
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    n = 200000
    res = run_func(data, 0x80010000, regs={"a0": n, "sp": 0x801000},
                   mem={0x801000: 0x80010000},
                   precall="g_dispatch_fn = gen_func_80010000;")
    assert res is not None
    assert res["r"][2] == (n * (n + 1) // 2) & 0xFFFFFFFF, hex(res["r"][2])



def test_exec_jalr_in_shared_tail_falls_through_to_epilogue():
    """A `jalr` inside a DUPLICATED shared tail must fall through to the instruction after its delay
    slot — here, the epilogue that restores s0.

    `jr` and `jalr` share the JUMPR decode kind, and collect_tail_dups used to end its walk on either.
    `jalr` writes ra with (addr+8) and control comes BACK to it, so ending the run there truncates the
    tail just before the epilogue; the emitter then closes the run with a bare `return;` and the
    `lw s0,8(sp)` never runs. Real occurrence: Spyro 0x800487D8 `jalr v0` with the state machine's own
    epilogue at 0x800487E0. s0 came back holding a stack pointer instead of the caller's saved value,
    and the caller used it as a loop counter -> ~62M iterations and a dead port (issue 0034).
    """
    if _skip_if_no_cc():
        return
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -16)
    a.sw("s0", 8, "sp")             # save the caller's s0
    a.addiu("s0", "zero", 0x77)     # clobber it, as a real case body does (`addiu s0, sp, 32`)
    a.j(0x80010018)                 # -> the shared tail, which lives PAST hi
    a.nop()
    a.nop()                         # pad so hi lands exactly on the tail
    a.label("tail")                 # 0x80010018
    a.jalr("v0")                    # a CALL through a register: ra = 0x80010020
    a.nop()
    a.lw("s0", 8, "sp")             # THE EPILOGUE — only runs if the jalr falls through
    a.addiu("sp", "sp", 16)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    res = run_func(data, 0x80010000, regs={"v0": 0x80055554, "sp": 0x801000, "s0": 0xCAFE},
                   hi=0x80010018)
    assert res["dispatch"] == 0x80055554, f"jalr did not call through v0: {res['dispatch']:#x}"
    assert res["r"][16] == 0xCAFE, \
        f"s0={res['r'][16]:#x} not restored — the tail ended AT the jalr, dropping the epilogue"


def test_exec_jalr_at_body_end_chains_into_next_fragment():
    """The same misclassification in body_falls_through(): a body whose LAST instruction is a `jalr`
    DOES reach `hi`, so emit_func must chain into the next fragment rather than emit a bare `return;`.
    (`jr` genuinely does not fall through, which is why the two must be told apart.)"""
    if _skip_if_no_cc():
        return
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -16)
    a.sw("s0", 8, "sp")
    a.addiu("s0", "zero", 0x77)
    a.jalr("v0")                    # last instruction of [lo, hi)
    a.nop()
    data, _ = a.assemble()
    hi = 0x80010014                 # the epilogue fragment, a separate function entry
    hooks = ("void func_80010014(Core* c){ c->r[16] = c->mem_r32(c->r[29] + 8); c->r[29] += 16; }\n")
    res = run_func(data, 0x80010000, regs={"v0": 0x80055554, "sp": 0x801000, "s0": 0xCAFE},
                   hi=hi, funcset={0x80010000, hi}, hooks=hooks)
    assert res["r"][16] == 0xCAFE, \
        f"s0={res['r'][16]:#x} — body ending in `jalr` did not chain into the fall-through fragment"



def emit_c(data, base=0x80010000, hi=None):
    """emit_func -> the C text, with no compile step. For asserting the SHAPE of an emission."""
    e = exe_of(data, base)
    out = []
    emit.emit_func(e, base, hi if hi is not None else e.text_end, {base}, out,
                   f"gen_func_{base:08X}", emit.MAIN_NAMES)
    return "\n".join(out)


def test_swc2_of_a_screen_xy_reg_taps_the_depth_recorder():
    """`swc2` of a GTE SCREEN-XY register is the packet-vertex store, and its address is the key the
    native-depth cache (ProjPrim) needs.

    WHY THIS IS THE WHOLE FEATURE. Native depth needs, per drawn vertex, the view-space Z that produced
    it. The recompiler already routes every perspective transform through one place, but the DEPTH has
    to be keyed by the address the guest wrote the projected X/Y to — that is what the renderer later
    looks up in gp0_exec. `gte_stsxy*` compiles to `swc2` of DR12/13/14 (and DR15 after an RTPS), so
    that store IS the seam, and it needs no per-game reverse engineering at all.

    The pairing is fixed by the GTE's FIFOs: XY DR12/13/14 pair with Z DR17/18/19, and DR15 (the RTPS
    single-vertex slot) pairs with DR19.

    `rt` is a compile-time constant in the emitted C, so a store of any OTHER cop2 register must emit
    the plain form — the tap costs nothing on the stores that are not vertices."""
    a = Asm(0x80010000)
    a.swc2(12, 0, "a0")        # screen XY of vertex 0 -> tapped, pairs with SZ DR17
    a.swc2(15, 8, "a0")        # RTPS single-vertex slot -> tapped, pairs with SZ DR19
    a.swc2(0, 16, "a0")        # VXY0: not a projected vertex -> must stay the plain store
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    assert "gte_store_xy(c, " in c, f"no depth tap emitted for swc2 of a screen-XY reg:\n{c}"
    assert c.count("gte_store_xy(c, ") == 2, \
        f"expected exactly 2 tapped stores (DR12, DR15), got {c.count('gte_store_xy(c, ')}:\n{c}"
    assert "gte_read_data(0)" in c, f"swc2 of a non-vertex reg must stay the plain store:\n{c}"



def test_mfc2_then_sw_taps_the_depth_recorder():
    """The OTHER vertex-submit idiom: `mfc2 rX, DR12..15` then `sw rX, off(base)`.

    A swc2-only depth tap records NOTHING for a game that submits this way, and records nothing
    SILENTLY — which is indistinguishable from a game with no 3D. Spyro is exactly that game: its main
    executable has zero `swc2` of the screen-XY registers and 70 `mfc2` of them (issue 0036).

    The pairing must be REFUSED in three cases, and each is a real correctness requirement rather than
    caution: the GPR being redefined before the store means the stored word is no longer that vertex; an
    intervening COP2 op has moved the Z FIFO on, so the depth would belong to a different vertex; and a
    basic-block boundary means the store is not reached unconditionally from the mfc2."""
    a = Asm(0x80010000)
    a.mfc2("v0", 12)                 # vertex 0's screen XY -> v0
    a.sw("v0", 0, "a0")              # TAPPED — pairs with Z DR17

    a.mfc2("v1", 13)
    a.addiu("v1", "zero", 5)         # v1 redefined: the stored word is not the vertex any more
    a.sw("v1", 4, "a0")              # not tapped

    a.mfc2("a2", 14)
    a.rtps()                         # SOFTWARE PIPELINING: the next vertex's transform is issued
    a.sw("a2", 8, "a0")              # STILL TAPPED — the Z was held at the mfc2, before the FIFO moved

    # A label reachable from OUTSIDE this walk — the branch to it is issued BEFORE the mfc2, so on
    # that path a3 never held a vertex at all. The register state on arrival is not the state this
    # walk reasoned about, so the identity cannot be assumed and the store must be refused.
    a.bne("a0", "zero", "joined")    # inbound edge from before the walk starts
    a.nop()
    a.mfc2("a3", 12)
    a.label("joined")
    a.sw("a3", 12, "a0")             # not tapped

    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    n = c.count("gte_record_pz(c, ")
    assert n == 2, f"expected 2 tapped vertex stores (the simple one and the pipelined one), got {n}:\n{c}"
    assert c.count("gte_hold_pz(c, ") == 2, f"each tapped vertex holds its Z at the mfc2:\n{c}"
    assert "gte_record_pz(c, (c->r[4] + (uint32_t)0), 2)" in c, \
        f"tap must carry the store address and the GPR whose Z was held (v0 = r2):\n{c}"




def test_lw_then_sw_propagates_depth_across_a_copy():
    """`lw rX, off(src)` then `sw rX, off2(dst)` — a word copy. If the loaded address carries a
    recorded vertex depth, the destination must inherit it.

    WHY THIS IS NEEDED. Spyro projects vertices into one buffer (much of it the scratchpad) and
    assembles the GP0 packets it actually DMAs in another, copying the screen XY across. Measured in a
    single frame: depths recorded at 0x1A86xx-0x1A8Dxx, packets drawn from 0x1AB7xx. Every lookup
    misses, and no amount of extra recording coverage can fix that — the depth is attached to the
    wrong copy of the value, so it has to follow the copy.

    The same refusal rules as the vertex tap apply: a redefinition of rX between the load and the
    store means a different word is being stored, and a control transfer means the store is not
    unconditionally reached.

    The runtime side must NOT fabricate depth: a source address with no recorded pz leaves the
    destination with none. Otherwise 2D elements that happen to copy words through the same registers
    acquire a world depth and sort themselves into the 3D scene."""
    a = Asm(0x80010000)
    a.lw("v0", 0, "a0")              # load from a tracked address …
    a.sw("v0", 0, "a1")              # … and store it: PROPAGATE
    a.lw("v1", 4, "a0")
    a.addiu("v1", "zero", 7)         # redefined: no longer the loaded word
    a.sw("v1", 4, "a1")              # must NOT propagate
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    n = c.count("gte_copy_pz(c, ")
    assert n == 1, f"expected exactly 1 propagated copy, got {n}:\n{c}"
    assert "gte_hold_src(c, 2, (c->r[4] + (uint32_t)0))" in c, \
        f"the source address must be HELD at the load (keyed by the seed GPR v0=2):\n{c}"
    assert "gte_copy_pz(c, 2, (c->r[5] + (uint32_t)0))" in c, \
        f"propagation must consume the held source and carry the destination address:\n{c}"



def test_vertex_survives_derivation_and_forward_branches():
    """Modeled literally on Spyro's terrain renderer, stage 1 (0x8004EDF8-0x8004EE44).

    The projected XY is NOT stored as it comes out of the GTE. It is shifted left 5 and the vertex's
    CLIP CODE is packed into the freed low bits by up to four conditional `addi`s, and only then
    stored to a scratchpad vertex cache. A tap that requires `mfc2 rX` -> `sw rX` with rX untouched
    sees none of it, which is why a renderer submitting ~1600 prims a frame recorded nothing.

    Two things have to hold. The value's IDENTITY must survive single-source derivations — a shift or
    an add-immediate still designates the same vertex. And the walk must cross the CONDITIONAL
    branches: they are forward jumps within this same run, so whichever path is taken, the store at
    the end stores this vertex. Crossing a branch is safe precisely because a not-taken store simply
    does not execute; what would be unsafe is a target reachable from OUTSIDE the run, carrying
    foreign register state."""
    a = Asm(0x80010000)
    a.mfc2("v0", 14)                  # projected XY of this vertex
    a.sll("a0", "v0", 5)              # shift up to make room for the clip code
    a.bgtz("a1", "c1")
    a.nop()
    a.addi("a0", "a0", 1)             # clip bit — conditional
    a.label("c1")
    a.bltz("a1", "c2")
    a.nop()
    a.addi("a0", "a0", 2)             # clip bit — conditional
    a.label("c2")
    a.sw("a0", 0, "s7")               # -> the scratchpad vertex cache. MUST be tapped.
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    assert c.count("gte_record_pz(c, ") == 1, \
        f"the derived, branch-crossed vertex store was not tapped:\n{c}"


def test_copy_source_address_is_captured_at_the_load():
    """Modeled on Spyro's terrain renderer, stage 2 (0x8004EF38-0x8004EF5C): the face list indexes the
    scratchpad cache, unshifts, and writes the packet.

    THE LOAD CLOBBERS ITS OWN BASE — `add t6,t6,s4` then `lw t6,0(t6)`. So re-evaluating the load's
    address expression at the STORE site reads the loaded VALUE as if it were a pointer, and attaches
    some unrelated word's depth. That is a wrong depth, which this code's own rule calls worse than
    none. The source address must be captured AT THE LOAD.

    The unshift (`sra`) between load and store is a derivation, not a kill — same identity."""
    a = Asm(0x80010000)
    a.add("t6", "t6", "s4")           # index the cache
    a.lw("t6", 0, "t6")               # load — DESTROYS the base register
    a.sra("t6", "t6", 5)              # unshift away the clip code
    a.sw("t6", 4, "fp")               # -> the GP0 packet
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    assert "gte_hold_src(c, " in c, f"the copy source address must be held at the load:\n{c}"
    # ORDER IS THE WHOLE POINT. The hold must be emitted BEFORE the load statement: `lw t6,0(t6)`
    # overwrites the very register the address is built from, so a hold placed after it captures the
    # LOADED VALUE as if it were an address. Same wrong-depth bug, one line later.
    hold_at = c.index("gte_hold_src(c, ")
    load_at = c.index("c->r[14] = c->mem_r32(")
    assert hold_at < load_at, \
        f"gte_hold_src must precede the load that clobbers its base register:\n{c}"
    assert c.count("gte_copy_pz(c, ") == 1, f"the packet store was not propagated to:\n{c}"
    # The propagation must NOT rebuild the source address from the (now clobbered) base register.
    assert "gte_copy_pz(c, 14," in c, \
        f"propagation must consume the HELD source (keyed by the seed GPR t6=14), not re-evaluate it:\n{c}"



def test_vertex_store_in_a_branch_delay_slot_is_tapped():
    """A vertex store sitting in a BRANCH DELAY SLOT must still be tapped.

    Hand-written PS1 assembly fills delay slots with real work, and these renderers put the vertex
    store there constantly: Spyro's main executable has 96 such stores, and in the single hottest
    renderer (0x800258F0) it is what breaks FOUR subdivided-face submitters end to end — their
    second stage is already tapped, so the whole chain is lost to one instruction per block.

    The emitter finds these stores; it simply had nowhere to append the record call, because a delay
    slot is emitted INSIDE the control statement. Appending to the delay-slot statement itself puts
    the call exactly where the hardware runs it: after the store, before the transfer."""
    a = Asm(0x80010000)
    a.mfc2("v0", 14)
    a.bne("a0", "zero", "skip")
    a.sw("v0", 0, "a1")              # THE DELAY SLOT — runs on both paths, and stores the vertex
    a.label("skip")
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    c = emit_c(data)
    assert c.count("gte_record_pz(c, ") == 1, \
        f"a vertex store in a delay slot was not tapped:\n{c}"
    # It must land INSIDE the control statement, after the store — not after the branch, where it
    # would run only on the fall-through path (or not at all).
    line = next(l for l in c.split("\n") if "gte_record_pz(c, " in l)
    assert "mem_w32" in line and "goto" in line, \
        f"the tap must sit in the delay-slot statement, between the store and the transfer:\n{line}"


# ----------------------------------------------------------------------------------------------------
def _main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    fails = 0
    for f in fns:
        try:
            f()
            print(f"ok   {f.__name__}")
        except AssertionError as e:
            fails += 1
            print(f"FAIL {f.__name__}: {e}")
        except Exception as e:  # noqa
            fails += 1
            print(f"ERR  {f.__name__}: {type(e).__name__}: {e}")
    print(f"\n{len(fns)-fails}/{len(fns)} passed")
    sys.exit(1 if fails else 0)




def test_demote_internal_labels_criterion():
    # RE-16: `jal` discovery promotes the internal block of a hand-written coroutine to a function
    # entry, splitting the real body; the emitter then renders an intra-function branch to it as
    # call+return and the enclosing function leaks its frame. Demotion fixes that, but ONLY if the
    # criterion can tell an internal label from three lookalikes that must be kept. All four shapes
    # below lack a stack prologue, which is why the old "no prologue AND fall-through" heuristic had
    # no discriminating power and demoted hundreds of entries.
    a = Asm(0x80010000)
    # H: a host body that CONDITIONALLY branches into `lbl` — fall-through stays in H, so both
    # successors belong to H and `lbl` is part of its control flow. THE demote case.
    a.addiu("sp", "sp", -0x10)      # 0x00  H (hard entry, has a prologue)
    a.bgez("zero", "lbl")           # 0x04  unconditional-in-practice, CONDITIONAL by encoding
    a.nop()                          # 0x08
    a.jal("lbl")                     # 0x0C  also jal'd — a mixed-reference internal block
    a.nop()                          # 0x10
    a.jal("sub")                     # 0x14  keeps `sub` a discovered entry
    a.nop()                          # 0x18
    a.j("tail")                      # 0x1C  TAIL CALL into `tail` — $ra is still H's caller's
    a.nop()                          # 0x20
    a.label("lbl")
    a.addu("v0", "v0", "v1")        # 0x24  internal label: no prologue, no way to return alone
    a.jr("ra")                       # 0x28
    a.nop()                          # 0x2C
    a.label("sub")
    a.addu("v0", "a0", "a1")        # 0x30  ordinary jal-only subroutine
    a.jr("ra")                       # 0x34
    a.nop()                          # 0x38
    a.label("tail")
    a.addu("v0", "a0", "zero")      # 0x3C  reached by `j` (tail call) AND `jal` below
    a.jr("ra")                       # 0x40
    a.nop()                          # 0x44
    a.jal("tail")                    # 0x48  the call that proves `tail` is a real function
    a.nop()                          # 0x4C
    data, _ = a.assemble()
    e = exe_of(data)
    H, lbl, sub, tail = 0x80010000, 0x80010024, 0x80010030, 0x8001003C
    got = emit.demote_internal_labels(e, [H, lbl, sub, tail], keep=set())
    assert got == {lbl}, f"expected only the branch-target label demoted, got {[hex(x) for x in sorted(got)]}"
    # Each spared case matters for a different reason, so assert them individually rather than as a set.
    assert sub not in got,  "a jal-only subroutine must never be demoted"
    assert tail not in got, "a `j` into a function is a TAIL CALL ($ra is the caller's) — not a label"
    assert H not in got,    "a hard entry with its own prologue must never be demoted"


def test_demote_spares_branch_and_link_targets():
    # bltzal/bgezal targets are CALL targets — discover_funcs seeds them deliberately (the 131-site
    # link-branch fix). They are branch-shaped, so a criterion that keys on "branched to" without
    # excluding the LINKING forms demotes them and reintroduces the exact frame leak it exists to
    # fix. Spider-Man has four such entries (0x8007CD44/D160/D1F0/D254), all link-only.
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -0x10)      # 0x00  H
    a.bltzal("a0", "linked")        # 0x04  CONDITIONAL *and* linking -> a call, not a branch
    a.nop()                          # 0x08
    a.jr("ra")                       # 0x0C
    a.nop()                          # 0x10
    a.label("linked")
    a.addu("v0", "v0", "v1")        # 0x14  no prologue, reached only by a link-branch
    a.jr("ra")                       # 0x18
    a.nop()                          # 0x1C
    data, _ = a.assemble()
    e = exe_of(data)
    got = emit.demote_internal_labels(e, [0x80010000, 0x80010014], keep=set())
    assert got == set(), f"a bltzal target is a real subroutine, got {[hex(x) for x in sorted(got)]}"


def _coroutine_image():
    """A minimal reproduction of Spider-Man 0x8002A338 (libpress `DecDCTvlc`, hand-written asm):
    an internal block that is BOTH `jal`-ed and branched to, whose `jr $ra` is the routine's own loop
    back-edge rather than a return.

        H:    addiu sp,sp,-4 / sw ra,(sp)      ; a frame it must give back
              jal  BLK                          ; $ra = A  -- an INTERNAL call
        A:    t1++ ; while (t1 < 3) goto L2
              b    EPI
        L2:   b    BLK                          ; UNCONDITIONAL INTRA-FUNCTION BRANCH; $ra still = A
        BLK:  v0 += 10
              jr   ra                           ; -> A. NOT a return.
        EPI:  lw ra,(sp) / addiu sp,sp,4 / jr ra

    Correct execution runs BLK three times: v0 == 30, and sp/ra come back exactly as they went in.
    The epilogue sits AFTER the coroutine `jr $ra`, as it does in the real routine — ra_computed_jumps
    walks a body in address order, so a test that put it earlier would answer "return" for a reason
    that has nothing to do with the wiring under test."""
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -4)         # 0x00
    a.sw("ra", 0, "sp")             # 0x04
    a.ori("v0", "zero", 0)          # 0x08
    a.ori("t1", "zero", 0)          # 0x0C
    a.jal("BLK")                    # 0x10  $ra = 0x18
    a.nop()                          # 0x14
    a.label("A")
    a.addiu("t1", "t1", 1)          # 0x18
    a.slti("at", "t1", 3)           # 0x1C
    a.bne("at", "zero", "L2")       # 0x20
    a.nop()                          # 0x24
    a.bgez("zero", "EPI")           # 0x28
    a.nop()                          # 0x2C
    a.label("L2")
    a.bgez("zero", "BLK")           # 0x30  the back-edge the emitter turns into call+return
    a.nop()                          # 0x34
    a.label("BLK")
    a.addiu("v0", "v0", 10)         # 0x38
    a.jr("ra")                       # 0x3C  coroutine resume, not a return
    a.nop()                          # 0x40
    a.label("EPI")
    a.lw("ra", 0, "sp")             # 0x44
    a.addiu("sp", "sp", 4)          # 0x48
    a.jr("ra")                       # 0x4C  the real return
    a.nop()                          # 0x50
    data, _ = a.assemble()
    return data


def test_exec_coroutine_internal_label_runs_its_loop_and_its_epilogue():
    # RE-16, end to end through emit_module. `jal` discovery promotes BLK to a function entry, which
    # splits the body; the unconditional branch to it at 0x30 is then emitted as `call + return`, so
    # the routine unwinds after ONE pass with its epilogue unrun. Measured on the real game before
    # the fix: 0x8002A338 returns with sp 4 low and $ra = 0x8002A424 (fntrace ABI check, validated
    # both ways), and the intro FMV decoder therefore produces one strip per movie and stops.
    if _skip_if_no_cc():
        return
    SP0, RA0 = 0x801FFF00, 0x80042424
    res = run_module(_coroutine_image(), 0x80010000, 0x80010000, regs={"sp": SP0, "ra": RA0})
    assert res["r"][2] == 30, \
        f"the coroutine's loop ran {res['r'][2] // 10} of 3 passes (v0={res['r'][2]}) — the " \
        f"intra-function branch to the internal block unwound the routine instead of jumping"
    assert res["r"][29] == SP0, \
        f"sp {SP0:08X} -> {res['r'][29]:08X}: the epilogue did not run, so the frame leaked"
    assert res["r"][31] == RA0, \
        f"ra {RA0:08X} -> {res['r'][31]:08X}: the internal link escaped as the caller's return address"
    # run_module intentionally enters the generated body directly; any wrapper reached through an
    # internal dispatch must nevertheless leave the shipping attribution stack balanced.
    assert res["otpush"] == res["otpop"] and res["otdepth"] == 0, \
        f"generated dispatch OT attribution was unbalanced: {res['otpush']} push, " \
        f"{res['otpop']} pop, depth {res['otdepth']}"


# ----------------------------------------------------------------------------------------------------
# 3. DATA-IMAGE GUARD — an overlay that is not code must emit an EMPTY module, never a recompiled
#    garbage blowup. Regression: Spyro's overlay scanner records every load into the shared arena,
#    including large DATA reads (OV_18F800 512KB, OV_20F800 480KB, OV_287800 75KB — Ghidra confirms
#    0 functions / 0% instruction coverage in all three). emit.py recompiled them as code: with no
#    jr-ra anywhere, every seeded "function" flood-filled the module, and OV_20F800 alone produced
#    ~144MB of C from 480KB of input.
# ----------------------------------------------------------------------------------------------------
def _garbage_image(nwords, base=0x8007AA38, seed=0x20F800):
    """Deterministic data-like bytes: high-entropy words with in-window `jal`s sprinkled in, so
    pre-fix discovery DOES seed functions inside the garbage, and — the whole point — no jr-ra and
    no stack prologue anywhere, so nothing ever terminates a flood-fill."""
    rng = random.Random(seed)
    words = []
    for _ in range(nwords):
        w = rng.getrandbits(32)
        if w == 0x03E00008 or (w & 0xFFFF8000) == 0x27BD8000:   # keep it return-free / prologue-free
            w ^= 0xFFFFFFFF
        words.append(w)
    for i in range(0, nwords, max(1, nwords // 8)):             # plant in-window jal seeds
        tgt = base + ((i * 137) % (nwords * 4)) & ~3
        words[i] = (3 << 26) | ((tgt >> 2) & 0x3FFFFFF)
    return struct.pack(f"<{len(words)}I", *words)


def test_data_image_is_not_code():
    e = exe_of(_garbage_image(4096), base=0x8007AA38)
    assert not emit.looks_like_code(e), \
        "return-free, prologue-free, largely-undecodable garbage must not classify as code"


def test_zero_filled_image_is_not_code():
    # All-nop padding decodes 0% unknown yet is still not code (OV_18F800's middle third was exactly
    # this). The unknown-fraction alone cannot be the criterion — the missing returns are what gives
    # data away.
    e = exe_of(b"\0" * 16384, base=0x8007AA38)
    assert not emit.looks_like_code(e), "a sea of nops has no callable function — not code"


def test_code_image_is_code():
    a = Asm(0x8007AA38)
    a.addiu("sp", "sp", -0x10)      # prologue
    a.sw("ra", 0x0C, "sp")
    a.jal("sub")
    a.nop()
    a.lw("ra", 0x0C, "sp")
    a.jr("ra")
    a.addiu("sp", "sp", 0x10)
    a.label("sub")
    a.addu("v0", "a0", "a1")
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    e = exe_of(data, base=0x8007AA38)
    assert emit.looks_like_code(e), "ordinary assembled functions must classify as code"


def test_data_overlay_emits_an_empty_module():
    """End-to-end through emit.py's CLI: a data .BIN under --overlays must produce a tiny EMPTY
    module (dispatch falls to rec_dispatch_miss) while KEEPING the overlay's router-table entry —
    the load is real and the runtime must still identify the resident image; it is just not code."""
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as td:
        a = Asm()                                   # minimal valid MAIN.EXE
        a.addiu("sp", "sp", -0x10)
        a.jr("ra")
        a.nop()
        text, _ = a.assemble()
        exe_path = os.path.join(td, "MAIN.EXE")
        hdr = bytearray(0x800)
        hdr[:8] = b"PS-X EXE"
        struct.pack_into("<II", hdr, 0x10, 0x80010000, 0)              # entry, gp
        struct.pack_into("<II", hdr, 0x18, 0x80010000, len(text))      # load, text size
        open(exe_path, "wb").write(bytes(hdr) + text)
        ovdir = os.path.join(td, "ovl")
        os.makedirs(ovdir)
        open(os.path.join(ovdir, "DATAOVL0.BIN"), "wb").write(_garbage_image(4096))   # 16KB of data
        seeds_path = os.path.join(td, "seeds.json")
        open(seeds_path, "w").write('{"overlay_bases": {"DATAOVL0": "0x8007AA38"}}')
        gen = os.path.join(td, "gen")
        os.makedirs(gen)
        r = subprocess.run([sys.executable, os.path.join(here, "emit.py"),
                            exe_path, os.path.join(gen, "rec.c"),
                            "--seeds", seeds_path, "--overlays", ovdir],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"emit.py failed:\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
        shards = [f for f in os.listdir(gen) if f.startswith("ov_dataovl0_shard_")]
        assert shards, "the data overlay still needs its module TUs (dispatch/index symbols are " \
                       "referenced by overlay_table.c)"
        total = sum(os.path.getsize(os.path.join(gen, f)) for f in shards)
        assert total < 4096, \
            f"data overlay emitted {total} bytes of C across {shards} — the garbage blowup is back"
        disp = open(os.path.join(gen, "ov_dataovl0_disp.c")).read()
        assert "case 0x" not in disp, "a data overlay must have ZERO dispatchable functions"
        table = open(os.path.join(gen, "overlay_table.c")).read()
        assert '"DATAOVL0"' in table, \
            "router identity must survive — the runtime still has to name the resident image"
        assert "NOT CODE" in r.stdout + r.stderr, \
            "a skipped data overlay must be ANNOUNCED, not silent — silence is how this hid"

def test_mixed_code_and_data_image_is_code():
    # Tomba!2's area overlays are the shape that kills a naive criterion: real functions (hundreds
    # of jr-ra) followed by embedded graphics/tables that decode 20-30% undecodable. The undecodable
    # fraction must NOT convict them — the returns are what counts.
    a = Asm(0x8007AA38)
    a.addiu("sp", "sp", -0x10)
    a.jal("sub")
    a.nop()
    a.jr("ra")
    a.nop()
    a.label("sub")
    a.addu("v0", "a0", "a1")
    a.jr("ra")
    a.nop()
    code, _ = a.assemble()
    data_tail = _garbage_image(2048)                      # 8KB of undecodable data after the code
    e = exe_of(code + data_tail, base=0x8007AA38)
    assert emit.looks_like_code(e), \
        "code with an embedded data section must stay CODE — Tomba!2's area overlays are this shape"


def _run_emit_cli(here, td, ov_bytes, ov_seeds=None):
    """Minimal emit.py CLI run with one overlay; returns (proc, gen_dir)."""
    a = Asm()
    a.addiu("sp", "sp", -0x10)
    a.jr("ra")
    a.nop()
    text, _ = a.assemble()
    exe_path = os.path.join(td, "MAIN.EXE")
    hdr = bytearray(0x800)
    hdr[:8] = b"PS-X EXE"
    struct.pack_into("<II", hdr, 0x10, 0x80010000, 0)
    struct.pack_into("<II", hdr, 0x18, 0x80010000, len(text))
    open(exe_path, "wb").write(bytes(hdr) + text)
    ovdir = os.path.join(td, "ovl")
    os.makedirs(ovdir, exist_ok=True)
    open(os.path.join(ovdir, "STAGE0.BIN"), "wb").write(ov_bytes)
    seeds = {"overlay_bases": {"STAGE0": "0x8007AA38"}}
    if ov_seeds:
        seeds["overlay_seeds"] = {"STAGE0": ov_seeds}
    seeds_path = os.path.join(td, "seeds.json")
    import json as _json
    open(seeds_path, "w").write(_json.dumps(seeds))
    gen = os.path.join(td, "gen")
    os.makedirs(gen, exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(here, "emit.py"),
                        exe_path, os.path.join(gen, "rec.c"),
                        "--seeds", seeds_path, "--overlays", ovdir],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"emit.py failed:\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
    return r, gen


def test_noreturn_code_needs_an_explicit_seed():
    """The one real-code shape with no jr-ra is a noreturn stage main (Tomba!2 START.BIN). With NO
    explicit seed the guard treats it as data (empty module, loudly); WITH one, the seed vouches for
    it and it is recompiled. Assert both arms — dropping a seeded overlay silently is the worse
    failure."""
    here = os.path.dirname(os.path.abspath(__file__))
    a = Asm(0x8007AA38)
    a.addiu("sp", "sp", -0x1C8)         # the START.BIN stage fn shape: prologue, no return
    a.label("loop")
    a.jal("worker")
    a.nop()
    a.b("loop")                          # noreturn main loop
    a.nop()
    a.label("worker")
    a.addu("v0", "a0", "zero")
    a.jalr("ra", "t9")                   # returns through t9, never a bare jr ra
    a.nop()
    ov, entry = a.assemble()
    # strip every jr-ra-shaped word just in case the assembler emitted one
    assert b"\x08\x00\xe0\x03" not in ov, "test image must contain no jr ra"
    with tempfile.TemporaryDirectory() as td:
        r, gen = _run_emit_cli(here, td, ov)
        disp = open(os.path.join(gen, "ov_stage0_disp.c")).read()
        assert "case 0x" not in disp, "seedless noreturn image: the data guard should empty it"
        assert "NOT CODE" in r.stdout + r.stderr
    with tempfile.TemporaryDirectory() as td:
        r, gen = _run_emit_cli(here, td, ov, ov_seeds=["0x8007AA38"])
        disp = open(os.path.join(gen, "ov_stage0_disp.c")).read()
        assert "case 0x" in disp, "an explicit seed vouches for noreturn code — it must recompile"


if __name__ == "__main__":
    _main()


def test_ra_computed_jump_vs_real_return():
    # RE-16: `jr $ra` does NOT always mean "return". Hand-written assembly can use `jal`/`jr $ra` as an
    # internal COROUTINE mechanism inside one frame, in which case $ra holds a mid-body resume point and
    # emitting `return;` unwinds out of the routine instead of resuming — dropping the rest of its work
    # and its epilogue. Spider-Man's 0x8002A338 (a resumable bit-stream decoder that saves its own
    # continuation to a global) has BOTH kinds of `jr $ra` in one body, so a whole-body gate is wrong
    # whichever way it answers. The decision must be per-`jr`, by reaching-definitions on $ra.
    a = Asm(0x80010000)
    a.addi("sp", "sp", -4)          # 0x00  frame
    a.sw("ra", 0, "sp")             # 0x04
    a.jal("sub")                    # 0x08  link = 0x10 — a live in-body resume point
    a.nop()                         # 0x0C  delay
    a.addu("v0", "v0", "v1")        # 0x10  <- the resume point
    a.b("tail")                     # 0x14
    a.nop()                         # 0x18
    a.label("sub")
    a.jr("ra")                      # 0x1C  COMPUTED: $ra is the jal link, not a return address
    a.nop()                         # 0x20  delay
    a.label("tail")
    a.lw("ra", 0, "sp")             # 0x24  epilogue reload
    a.addi("sp", "sp", 4)           # 0x28
    a.jr("ra")                      # 0x2C  REAL RETURN
    a.nop()                         # 0x30  delay
    data, _ = a.assemble()
    e = exe_of(data)
    got = emit.ra_computed_jumps(e, [0x80010000])
    assert got == {0x8001001C}, \
        f"expected only the coroutine jr to be computed, got {[hex(x) for x in sorted(got)]}"

    # The emitter must render the two differently: a router dispatch vs a bare `return;`.
    ins = {x: decode(x, e.word(x)) for x in range(0x80010000, 0x80010034, 4)}
    NM = emit.MAIN_NAMES
    coro = emit.emit_control(ins[0x8001001C], "", set(), set(), NM, None, False, True)
    real = emit.emit_control(ins[0x8001002C], "", set(), set(), NM, None, False, False)
    assert real[0].strip() == "return;", f"real return should be a bare return, got {real}"
    assert NM.router in coro[0] and "r[31]" in coro[0], \
        f"coroutine resume should dispatch on $ra, got {coro}"


def test_ra_saved_to_a_global_is_still_a_return():
    # "Not the stack" is not the same as "not a return address". A FRAMELESS function parks its return
    # address in a GLOBAL and reloads it (Spider-Man 0x8008BE5C: `sw $ra, 0x392C($at)` … `lw $ra,
    # 0x392C($ra)`). Treating any non-`sp` reload as a continuation reported four such functions as
    # coroutines. The discriminator is whether THIS function ever stored $ra to that offset.
    a = Asm(0x80010000)
    a.lui("at", 0x800B)             # 0x00
    a.sw("ra", 0x392C, "at")        # 0x04  save slot — a global, not the stack
    a.jal("callee")                 # 0x08
    a.nop()                         # 0x0C
    a.lui("ra", 0x800B)             # 0x10
    a.lw("ra", 0x392C, "ra")        # 0x14  reload — a RETURN address
    a.nop()                         # 0x18
    a.jr("ra")                      # 0x1C  REAL RETURN, despite the jal above it
    a.nop()                         # 0x20
    a.label("callee")
    a.jr("ra")                      # 0x24
    a.nop()                         # 0x28
    data, _ = a.assemble()
    e = exe_of(data)
    got = emit.ra_computed_jumps(e, [0x80010000, 0x80010024])
    assert got == set(), f"a global save slot is still a return, got {[hex(x) for x in sorted(got)]}"

    # And the guard is what saves it: without the `sw` the same reload IS a restored continuation.
    b = Asm(0x80010000)
    b.lui("ra", 0x800B)
    b.lw("ra", 0x392C, "ra")
    b.nop()
    b.jr("ra")
    b.nop()
    d2, _ = b.assemble()
    got2 = emit.ra_computed_jumps(exe_of(d2), [0x80010000])
    assert got2 == {0x8001000C}, \
        f"a reload from a slot this function never saved to is a continuation, got {got2}"


def test_ra_global_save_slot_survives_a_partition_split():
    # SPYRO 0x80022A2C, verbatim shape. A FRAMELESS renderer spills its callee-saved registers AND
    # `$ra` to a fixed GLOBAL block (0x80077DD8 + 44) and reloads them in its epilogue. The emitter's
    # partition is the emitted-function list, not the guest function, and this body gets SPLIT — its
    # `overlay_funcs` jal-scan promoted 0x80023384, a `nop` in the delay slot of `j`, to a function
    # entry. So the prologue's `sw $ra` lands in a DIFFERENT partition entry from the epilogue's
    # `lw $ra`, a per-entry save-slot test cannot see it, and a plain return was emitted as
    # `rec_dispatch(c, ra)` into the CALLER's mid-function return address -> recomp-MISS -> SIGABRT
    # 3544 frames into the run (spyro docs/issues/0046).
    #
    # A GLOBAL save area is shared across guest functions BY CONSTRUCTION, so "the same entry stored
    # to this offset" is unsound for one. The store has to be looked for MODULE-WIDE, keyed by the
    # resolved link-time base as well as the offset.
    a = Asm(0x80010000)
    a.lui("at", 0x8007)             # 00  the global register-save block ...
    a.addiu("at", "at", 0x7DD8)     # 04  ... = 0x80077DD8
    a.sw("ra", 44, "at")            # 08  PROLOGUE: park the caller's return address in it
    a.jal("helper")                 # 0C  ordinary call — clobbers $ra
    a.nop()                         # 10
    a.j("body")                     # 14  the split point is this jump's DELAY SLOT
    a.nop()                         # 18  <- 0x80010018: seeded as a "function", splitting the body
    a.label("body")
    a.lui("at", 0x8007)             # 1C  EPILOGUE
    a.addiu("at", "at", 0x7DD8)     # 20
    a.lw("ra", 44, "at")            # 24  reload — a RETURN address, not a continuation
    a.nop()                         # 28
    a.jr("ra")                      # 2C  ORDINARY RETURN
    a.nop()                         # 30
    a.label("helper")
    a.jr("ra")                      # 34
    a.nop()                         # 38
    data, _ = a.assemble()
    e = exe_of(data)
    # The BAD partition is the point: 0x80010018 is a delay-slot `nop`, not a function.
    assert not emit.is_func_entry(e, 0x80010018), "the split point must not look like an entry"
    got = emit.ra_computed_jumps(e, [0x80010000, 0x80010018, 0x80010034])
    assert got == set(), \
        ("a global save slot is a RETURN even when the partition splits the store away from the "
         f"load, got {[hex(x) for x in sorted(got)]}")


def test_ra_a_jal_on_a_skipped_path_does_not_poison_a_return():
    # SPYRO 0x80053570, verbatim shape. The `bne` jumps OVER the `jal`; the `jal` path leaves through
    # its own `jr $a3` and never reaches the `jr $ra`. So $ra is untouched on EVERY path that reaches
    # the return — but the shipped analysis is a LINEAR SWEEP in address order (its docstring claims
    # reaching-definitions), so it walks past the `jal` it can never have executed and calls the
    # return a coroutine resume. The fix is an actual forward fixpoint over the basic-block graph.
    a = Asm(0x80010000)
    a.bne("v0", "a1", "skipped")    # 00  taken -> the jal below never runs
    a.nop()                         # 04
    a.addi("a3", "ra", 0)           # 08  the jal path saves $ra in $a3 ...
    a.jal("helper")                 # 0C  ... calls ...
    a.nop()                         # 10
    a.jr("a3")                      # 14  ... and tail-returns through $a3. It NEVER falls through.
    a.nop()                         # 18
    a.label("skipped")
    a.jr("ra")                      # 1C  ORDINARY RETURN — $ra is the caller's, untouched
    a.nop()                         # 20
    a.label("helper")
    a.jr("ra")                      # 24
    a.nop()                         # 28
    data, _ = a.assemble()
    e = exe_of(data)
    got = emit.ra_computed_jumps(e, [0x80010000, 0x80010024])
    assert got == set(), \
        ("a `jal` the control flow skips cannot define the $ra a later `jr` reads, got "
         f"{[hex(x) for x in sorted(got)]}")


def test_ra_save_slot_is_matched_by_BASE_not_just_offset():
    # The save-slot test has to compare the ADDRESS, not the displacement. Two unrelated globals
    # sharing an offset is ordinary (every one of Spyro's save blocks uses +44), so matching on the
    # offset alone lets a store to block A vouch for a load from block B — and a restored
    # CONTINUATION then reads as a return, which is the failure in the other direction.
    a = Asm(0x80010000)
    a.lui("at", 0x8007)             # 00
    a.addiu("at", "at", 0x7DD8)     # 04  block A = 0x80077DD8
    a.sw("ra", 44, "at")            # 08  store to A+44
    a.lui("t0", 0x8009)             # 0C
    a.addiu("t0", "t0", 0x1234)     # 10  block B = 0x80091234 — a DIFFERENT object
    a.lw("ra", 44, "t0")            # 14  load from B+44: nothing ever stored $ra there
    a.nop()                         # 18
    a.jr("ra")                      # 1C  a restored CONTINUATION, not a return
    a.nop()                         # 20
    data, _ = a.assemble()
    got = emit.ra_computed_jumps(exe_of(data), [0x80010000])
    assert got == {0x8001001C}, \
        ("a store to a different global must not vouch for this load, got "
         f"{[hex(x) for x in sorted(got)]}")
