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
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
import decode as D
from decode import decode
import psexe
import emit

SCRATCH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "scratch", "bin"))


def scratch_tempdir(prefix="emit-test-"):
    os.makedirs(SCRATCH, exist_ok=True)
    return tempfile.TemporaryDirectory(prefix=prefix, dir=SCRATCH)

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
            # `jalr rs` (rd=$ra) or `jalr rd, rs` — CTR's GTE library links calls through t2
            if len(args) == 2:
                return R(_r(args[1]), 0, _r(args[0]), 0, 9)
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
    with scratch_tempdir() as legacy, scratch_tempdir() as explicit:
        assert _emit_checkpoint_fixture(legacy) == _emit_checkpoint_fixture(explicit, set())


def test_generated_raw_override_getter_reads_the_exact_slot():
    with scratch_tempdir() as td:
        files = _emit_checkpoint_fixture(td)
        header = files[emit.MAIN_NAMES.decls].decode()
        dispatch = files[emit.MAIN_NAMES.disp + ".c"].decode()

        assert "OverrideFn shard_get_override(uint32_t addr);" in header
        assert (
            "OverrideFn shard_get_override(uint32_t addr) "
            "{ int i = rec_func_index(addr); return i >= 0 ? g_override[i] : nullptr; }"
            in dispatch
        )


def test_syscall_emission_carries_the_exact_guest_pc():
    instruction = decode(0x80012340, (0x54321 << 6) | 0x0C)
    assert emit.emit_simple(instruction) == "rec_syscall(c, 344865u, 0x80012340u);"


def test_diagnostic_checkpoints_precede_two_selected_ordinary_instructions():
    with scratch_tempdir() as td:
        files = _emit_checkpoint_fixture(td, {0x80010000, 0x80010004})
        shard = files[emit.MAIN_NAMES.shardpfx + "_0.c"].decode()
        assert shard.count("pc_observer_at(c,") == 2
        assert "pc_observer_at(c, 0x80010000u);\n  c->r[2] =" in shard
        assert "pc_observer_at(c, 0x80010004u);\n  c->r[3] =" in shard


def test_diagnostic_checkpoint_branch_target_observes_after_label_before_instruction():
    a = Asm()
    a.beq("zero", "zero", "target")
    a.nop()
    a.addiu("v0", "zero", 1)
    a.label("target")
    a.addiu("v1", "zero", 2)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    with scratch_tempdir() as td:
        emit.emit_module(exe_of(data), td, emit.MAIN_NAMES, {a.base}, shards=1,
                         diagnostic_pcs={a.labels["target"]})
        shard = open(os.path.join(td, emit.MAIN_NAMES.shardpfx + "_0.c")).read()
        pc = a.labels["target"]
        assert f"L_{pc:08X}:;\n  pc_observer_at(c, 0x{pc:08X}u);\n  c->r[3] =" in shard


def test_diagnostic_checkpoint_overlapping_bodies_refuse_duplicate_site():
    a = Asm()
    a.beq("zero", "zero", "shared")
    a.nop()
    a.jr("ra")
    a.nop()
    second = a.base + 16
    a.addiu("v0", "zero", 1)
    a.nop()
    a.label("shared")
    a.addiu("v1", "zero", 2)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    with scratch_tempdir() as td:
        try:
            emit.emit_module(exe_of(data), td, emit.MAIN_NAMES, {a.base, second}, shards=1,
                             diagnostic_pcs={a.labels["shared"]})
        except SystemExit as error:
            message = str(error)
            assert "requested_targets=1" in message and "emitted_sites=2" in message
            assert f"*@0x{a.labels['shared']:08X}(2)" in message
        else:
            assert False, "a requested PC duplicated into overlapping generated bodies must refuse"


def test_function_qualified_checkpoint_selects_one_overlapping_body():
    a = Asm()
    a.beq("zero", "zero", "shared")
    a.nop()
    a.jr("ra")
    a.nop()
    second = a.base + 16
    a.addiu("v0", "zero", 1)
    a.nop()
    a.label("shared")
    a.addiu("v1", "zero", 2)
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    with scratch_tempdir() as td:
        emit.emit_module(exe_of(data), td, emit.MAIN_NAMES, {a.base, second}, shards=1,
                         diagnostic_pcs={(second, a.labels["shared"])})
        shard = open(os.path.join(td, emit.MAIN_NAMES.shardpfx + "_0.c")).read()
        assert shard.count(f"pc_observer_at(c, 0x{a.labels['shared']:08X}u);") == 1


def test_diagnostic_checkpoint_outside_emitted_text_refuses_with_denominator():
    with scratch_tempdir() as td:
        try:
            _emit_checkpoint_fixture(td, {0x80010100})
        except SystemExit as error:
            message = str(error)
            assert "requested_targets=1 emitted_sites=0" in message
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





def _cross_overlay_fixture():
    """Two co-resident overlay images plus a same-slot sibling.

    DEST holds a null handler T (`jr ra` + delay slot) that a NEIGHBOURING body branches to — the
    shape demote_internal_labels proves an internal label — while SRC, a disjoint image, `jal`s it
    directly. SIB overlaps DEST (same slot, so the two can never be resident together) and holds
    ordinary arithmetic at T's address, i.e. no callable entry there."""
    d = Asm(0x80100000)
    d.addiu("sp", "sp", -0x10)      # 0x00  a real function, so T is preceded by `jr ra` + delay
    d.jr("ra")                      # 0x04
    d.nop()                         # 0x08
    d.label("T")
    d.jr("ra")                      # 0x0C  T: the null handler SRC calls
    d.addiu("t0", "zero", 7)        # 0x10  delay slot
    d.addiu("sp", "sp", -0x10)      # 0x14  the neighbouring body...
    d.beq("a0", "zero", "T")        # 0x18  ...which branches BACK into T (non-linking, conditional)
    d.nop()                         # 0x1C
    d.jal(0x80100030)               # 0x20  into SIB's range — but they SHARE a slot, so this call
    d.nop()                         # 0x24  is not executable and must not seed SIB
    d.jr("ra")                      # 0x28
    d.nop()                         # 0x2C
    dest, _ = d.assemble()
    T = 0x8010000C

    s_ = Asm(0x80200000)
    s_.addiu("sp", "sp", -0x10)
    s_.jal(T)                       # the cross-image direct call — the evidence T is an entry
    s_.nop()
    s_.jr("ra")
    s_.nop()
    src, _ = s_.assemble()

    b = Asm(0x80100000)             # SAME base as DEST: the other image in that slot
    for _ in range(12):
        b.addu("v0", "v0", "v1")    # nothing at T's address that could be an entry
    b.addiu("sp", "sp", -0x10)      # 0x30: a PERFECTLY VALID entry here — rejected on the slot proof
    for _ in range(3):
        b.addu("v0", "v0", "v1")
    sib, _ = b.assemble()

    return (T,
            exe_of(dest, 0x80100000),
            exe_of(src, 0x80200000),
            exe_of(sib, 0x80100000))


def test_cross_overlay_call_target_seeds_only_the_co_resident_image():
    # Issue #22 (Vagrant): INITBTL `jal`s BATTLE 0x800E6EAC, which BATTLE's own 0x800E6EB4 also
    # branches to. Each module's jal scan is internal, so the destination never discovered the entry,
    # and demote_internal_labels then dropped it as a local label — INITBTL's call fail-fasted with
    # `recomp-MISS 0x800E6EAC`. The two filters must accept the co-resident image and reject the
    # same-slot sibling that spans the same address.
    T, dest, src, sib = _cross_overlay_fixture()
    seeds = emit.cross_overlay_call_targets([("DEST", dest), ("SIB", sib), ("SRC", src)])
    assert seeds["DEST"] == {T}, f"co-resident destination not seeded: {seeds}"
    assert seeds["SIB"] == set(), f"same-slot sibling wrongly seeded: {seeds}"
    assert seeds["SRC"] == set(), f"caller wrongly seeded: {seeds}"
    # And the slot proof carries its own weight: DEST calls a REAL entry inside SIB, which the entry
    # test would happily accept — it is rejected because the two images can never be resident together.
    assert emit.is_func_entry(sib, 0x80100030), "fixture must offer SIB a genuine entry to reject"


def test_cross_overlay_call_target_survives_internal_label_demotion():
    # The dual role: the SAME address is a callable entry (a cross-image `jal` names it) and a branch
    # target of a neighbouring body. Without the cross-image seed, rule 1 of demote_internal_labels
    # proves it an internal label and the destination module emits no callable entry for it.
    T, dest, src, sib = _cross_overlay_fixture()
    soft = emit.func_entries_after_return(dest)
    assert T in soft, "fixture must present T as a function boundary in the first place"
    internal_only = emit.overlay_internal_jal_targets(dest)
    assert T not in internal_only, "fixture must not discover T from inside DEST"

    entry = f"ov_dest_func_{T:08X}"
    with scratch_tempdir() as td:
        emit.emit_module(dest, td, emit.overlay_names("dest"), internal_only, None, None,
                         shards=1, soft_seeds=soft)
        without = open(os.path.join(td, "ov_dest_disp.c")).read()
    assert entry not in without, "control failed: T was already a callable entry without the seed"

    seeds = emit.cross_overlay_call_targets([("DEST", dest), ("SIB", sib), ("SRC", src)])
    with scratch_tempdir() as td:
        emit.emit_module(dest, td, emit.overlay_names("dest"), internal_only | seeds["DEST"], None,
                         None, shards=1, soft_seeds=soft)
        with_seed = open(os.path.join(td, "ov_dest_disp.c")).read()
        body = open(os.path.join(td, "ov_dest_shard_0.c")).read()
    assert entry in with_seed, "cross-image call target is still not dispatchable in the destination"
    # The neighbouring body must still REACH T — as a call to the entry or as its own local label.
    assert (f"{entry}(c)" in body or f"L_{T:08X}" in body), \
        "the neighbouring body lost its transfer into T"


def test_delay_slot_on_a_function_boundary_is_still_emitted():
    # A delay slot belongs to its CONTROL INSTRUCTION, not to an address range. When the next function
    # entry starts ON the slot — the compiler filled the preceding `jr ra`'s slot with that entry's
    # first instruction, and something `jal`s the entry — the slot sits outside the owning function's
    # [lo,hi). Emitting nothing there DROPS a real guest instruction, silently. Measured on Vagrant:
    # 28 emitted functions ended on a control instruction whose slot was a live non-nop word,
    # including a `sh` store (0x80082C0C) — a guest memory write that never happened.
    a = Asm(0x80010000)
    a.addiu("sp", "sp", -0x10)      # 0x00  fn H
    a.jr("ra")                      # 0x04  H's epilogue...
    a.lui("t0", 0x800F)             # 0x08  ...whose delay slot is ALSO the next entry
    a.addiu("t0", "t0", 0x1928)     # 0x0C  the rest of that next function
    a.jr("ra")                      # 0x10
    a.nop()                         # 0x14
    data, _ = a.assemble()
    e = exe_of(data)
    H, G = 0x80010000, 0x80010008

    with scratch_tempdir() as td:
        emit.emit_module(e, td, emit.MAIN_NAMES, {H, G}, shards=1)
        body = open(os.path.join(td, "shard_0.c")).read()
    h = body.split("void gen_func_80010000")[1].split("void gen_func")[0]
    assert "32783" in h or "0x800F" in h.upper(), \
        f"H's delay slot (lui t0,0x800F) was dropped at the function boundary:\n{h}"


def _mask_stride_fixture(base=0x80010000, with_jal_twin=False):
    """The Vagrant resident-libgte dispatcher shape (recomp-MISS 0x80040FC8).

    D computes its jump target as `srl t1,t8,4 ; andi t1,t1,0x60 ; addu t1,t1,t7 ; jr t1` — the
    POST-SCALE MASK family: the reachable offsets are exactly the multiples of 0x20 inside 0x60,
    i.e. four 32-byte run blocks R0..R3. Each block ends in `jr ra`, so func_entries_after_return
    soft-seeds every block start; a leaf tail before R0 makes R0 such a candidate too."""
    d = Asm(base)
    RUN = base + 11 * 4             # eleven D-instructions precede the run; R0 sits there
    d.addiu("sp", "sp", -8)         # D: prologue
    d.lw("t8", 0, "a0")             # dynamic index source
    d.lui("t7", RUN >> 16)
    d.addiu("t7", "t7", RUN & 0xFFFF)
    d.srl("t1", "t8", 4)
    d.andi("t1", "t1", 0x60)        # stride 0x20, 4 slots
    d.addu("t1", "t1", "t7")
    d.jr("t1")
    d.nop()
    d.jr("ra")                      # leaf tail of D's body -> the next word looks like an entry
    d.nop()
    for k in range(4):              # R0..R3: four 32-byte blocks, each returns
        d.addiu("v0", "v0", 1 + k)
        d.sw("v0", 0, "a1")
        d.lw("v1", 4, "a1")
        d.addiu("v1", "v1", 1)
        d.sw("v1", 4, "a1")
        d.addiu("a2", "a2", k)
        d.jr("ra")
        d.nop()                     # 8 words = 32 bytes per block
    data, end = d.assemble()
    if with_jal_twin:
        g = Asm(end)
        g.jal(RUN + 0x40)           # direct call evidence for R2 -> it must survive pruning
        g.jr("ra")
        g.nop()
        data += g.assemble()[0]
    return exe_of(data, base), RUN


def test_masked_shifted_index_dispatcher_recovers_mask_stride_cases():
    # The post-scale mask IS the case bound: `srl other,X,k ; andi other,other,M` reaches exactly
    # the multiples of M's lowest set bit below M. Measured on Vagrant: resident libgte dispatches
    # through `srl t1,t8,4 ; andi t1,t1,0xE0 ; addu t1,t1,t7 ; jr t1` into eight 32-byte macro
    # blocks; the generic run-walk reads only two instructions per probe at stride 8 and proves
    # nothing there, so recovery returned None and the hop became a runtime register dispatch.
    e, run = _mask_stride_fixture()
    ins = {a: decode(a, e.word(a)) for a in range(e.load, e.text_end, 4)}
    got = emit.find_jump_tables(e, ins, e.load, e.text_end)
    jrs = [a for a, t in got.items()]
    targets = set()
    for t in got.values():
        targets.update(t or ())
    expected = {run + k * 0x20 for k in range(4)}
    assert expected <= targets, (
        f"mask-stride cases missing: want "
        f"{', '.join(f'{t:08X}' for t in sorted(expected))}; got "
        f"{', '.join(f'{a:08X}' for a in jrs)} -> {sorted(targets)}")


def test_soft_entry_inside_recovered_run_is_pruned_but_call_evidence_survives():
    # A soft-discovered boundary that sits INSIDE a recovered computed-jump run is one of its case
    # labels, not a function — leaving it in splits the containing body so neither the outer
    # dispatcher nor the inner block-to-block hops recover (the Vagrant 0x80041104 split). But a
    # direct `jal` names a REAL entry even mid-run: that twin must survive.
    for with_twin, want_case in ((False, False), (True, True)):
        e, run = _mask_stride_fixture(with_jal_twin=with_twin)
        twin = run + 0x40
        hard = {e.load}                       # only the image entry is hard here
        soft = emit.func_entries_after_return(e)
        assert run in soft, "fixture must present the first run block as a soft boundary"
        with scratch_tempdir() as td:
            emit.emit_module(e, td, emit.MAIN_NAMES, hard, shards=1, soft_seeds=soft)
            disp = open(os.path.join(td, "shard_disp.c")).read()
            body = open(os.path.join(td, "shard_0.c")).read()
        case = f"case 0x{twin & 0x1FFFFFFF:08X}u"   # main_dispatch switches on the 27-bit address
        if want_case:
            assert case in disp, \
                f"direct-called twin wrongly pruned out of the callable set:\n{disp[:2000]}"
        else:
            assert case not in disp, \
                f"case label left a callable entry — the containing body stays truncated:\n{disp[:2000]}"
            assert f"L_{twin:08X}" in body, "pruned case label lost from the containing body"


def test_constructed_pointer_that_feeds_a_computed_jump_is_not_a_function_seed():
    # The Vagrant resident-libgte shape (recomp-MISS 0x80040FC8): the dispatcher materialises its
    # jump-table BASE with `lui t7,0x8004 ; addiu t7,t7,0x1104` and consumes it through
    # add-with-index into `jr t1`. constructed_func_pointers saw the same lui/addiu pair, liked
    # is_func_entry at the base address (the preceding block ends jr ra + delay), and HARD-seeded
    # the run's first case block — truncating the containing body so neither the outer dispatcher
    # nor any inner hop could recover. A value whose sink is a computed JUMP is a jump base, not a
    # callback; only a store or jalr sink makes it a function pointer.
    d = Asm(0x80010000)
    d.lw("t8", 0, "a0")
    d.lui("t7", 0x8001)             # base = 0x8001002C: the first case block below
    d.addiu("t7", "t7", 0x2C)
    d.srl("t1", "t8", 4)
    d.andi("t1", "t1", 0x60)
    d.addu("t1", "t1", "t7")
    d.jr("t1")
    d.nop()
    d.jr("ra")                      # block boundary -> is_func_entry(0x8001002C) true
    d.nop()
    for _ in range(3):              # three case blocks, each returning
        d.addiu("v0", "v0", 1)
        d.jr("ra")
        d.nop()
    data, _ = d.assemble()
    e = exe_of(data)
    assert (0x8001002C) not in emit.constructed_func_pointers(e), \
        "jump-table base hard-seeded as a constructed function pointer"

    # Control: the SAME kind of value STORED as a callback keeps seeding — the sink decides.
    c = Asm(0x80010000)
    c.lui("t7", 0x8001)
    c.addiu("t7", "t7", 0x14)       # value = the entry-shaped word below
    c.sw("t7", 0, "a0")             # stored into a vtable/callback slot
    c.jr("ra")
    c.nop()
    c.addiu("sp", "sp", -8)         # entry-shaped word at 0x80010014
    c.jr("ra")
    c.nop()
    data, _ = c.assemble()
    e = exe_of(data)
    assert (0x80010014) in emit.constructed_func_pointers(e), \
        "stored constructed pointer lost its function seed"


def _fragment_chain_fixture(base=0x80010000):
    """The issue-#23 shape: a dispatch base carried ACROSS a `jr ra` fragment boundary.

    Block A materialises t9 = TGT and branches into block B (the analyzer walks branch targets as
    edges exactly as it walks recovered-switch cases). B sits behind a jr-ra boundary, consumes t9
    with `addiu t9,t9,32` and jumps: the definite target TGT+32 can only come from cross-fragment
    flow."""
    d = Asm(base)
    d.lui("t9", base >> 16)         # 0x00 A: t9 = TGT
    d.addiu("t9", "t9", 0x60)       # 0x04
    d.beq("zero", "zero", "B")      # 0x08 transfer into B — an edge the analyzer walks
    d.nop()                         # 0x0C delay
    d.jr("ra")                      # 0x10 boundary filler: B looks like a fresh entry
    d.nop()                         # 0x14
    assert len(d.items) == 6        # B begins at the next word, base+0x18
    d.label("B")
    d.addiu("t9", "t9", 32)         # 0x18 B: t9 = TGT+32 — base flows in from A
    d.jr("t9")                      # 0x1C the unrecovered computed jump under test
    d.nop()                         # 0x20
    d.jr("ra")                      # 0x24
    d.nop()                         # 0x28
    while len(d.items) < 24:        # filler up to TGT = base+0x60
        d.addu("s0", "s0", "s0")
    d.label("TGT")
    d.addiu("v0", "v0", 1)          # base+0x60
    d.addiu("v0", "v0", 2)
    d.jr("ra")
    d.nop()
    for _ in range(5):              # keep the flow-proven TGT+32 inside the executable image
        d.addu("s1", "s1", "s1")
    d.label("TGT32")
    d.addiu("v1", "v1", 1)
    d.jr("ra")
    d.nop()
    data, _ = d.assemble()
    return exe_of(data, base), base + 0x60, base + 0x80


def test_cross_fragment_dispatch_base_is_recovered_and_seeded():
    # The register holding a computed-jump target can be materialised in an EARLIER fragment and
    # consumed after a `jr ra` boundary (Sony's hand-written GTE chains pass registers across what
    # look like function ends). Must-constant flow along the control graph — sequential edges plus
    # branch/recovered-switch targets — recovers the definite value where per-function analysis
    # structurally cannot. Measured on Vagrant BATTLE: recomp-MISS 0x800B182C from jr t9 @0x800B1704.
    e, tgt, tgt32 = _fragment_chain_fixture()
    sites = emit.unrecovered_jr_targets(
        e, funcs=[e.load], entries={e.load}, jt_edges={})
    assert sites, "analyzer found no unrecovered jr site at all"
    a, consts = next(iter(sites.items()))
    assert tgt32 in consts, (
        f"cross-fragment base lost: site {a:#x} consts={sorted(hex(c) for c in consts)}")

    with scratch_tempdir("emit-cross-fragment-") as td:
        emit.emit_module(e, td, emit.MAIN_NAMES, {e.load, e.load + 0x18}, shards=1)
        disp = open(os.path.join(td, "shard_disp.c")).read()
    assert f"case 0x{tgt32 & 0x1FFFFFFF:08X}u:" in disp, \
        "flow-proven cross-fragment target was not emitted as a dispatchable entry"


def test_module_wide_switch_guess_is_not_seeded_without_flow_evidence():
    # Module-wide switch recovery is deliberately permissive: its targets are graph EDGES that let
    # constant flow cross hand-written GTE fragments, not proof that every guessed target is a
    # callable entry. Spyro's jr s1 @0x8004D2D8 produced 50 module-wide candidates while its local
    # span proved only five; wholesale seeding made the unrelated 0x8004D710 branch delay slot a
    # function and executed its `sll t1,t1,2` twice on fallthrough.
    d = Asm(0x80010000)
    d.addiu("t0", "zero", 0)
    d.jr("t0")
    d.nop()
    d.addiu("v0", "v0", 1)
    d.beq("zero", "zero", "done")
    d.label("false_entry")
    d.sll("t1", "t1", 2)  # branch delay slot: valid code, but not a callable entry
    d.label("done")
    d.jr("ra")
    d.nop()
    data, _ = d.assemble()
    e = exe_of(data)
    jr_site = e.load + 4
    false_entry = d.labels["false_entry"]

    real_find = emit.find_jump_tables

    def global_guess_only(exe, ins, lo, hi, validate=True, tbl_spans=None):
        if lo == e.load and hi == e.text_end:
            return {jr_site: [false_entry]}
        return real_find(exe, ins, lo, hi, validate=validate, tbl_spans=tbl_spans)

    with scratch_tempdir("emit-global-guess-") as td, \
            mock.patch.object(emit, "find_jump_tables", side_effect=global_guess_only), \
            mock.patch.object(emit, "unrecovered_jr_targets", return_value={}):
        emit.emit_module(e, td, emit.MAIN_NAMES, {e.load}, shards=1)
        disp = open(os.path.join(td, "shard_disp.c")).read()
        body = open(os.path.join(td, "shard_0.c")).read()

    assert f"func_{false_entry:08X}" not in disp, \
        "unproven module-wide switch guess became a callable entry"
    assert "c->r[9] = c->r[9] << 2;" in body, \
        "the branch delay slot disappeared instead of remaining in its containing body"


def test_cross_fragment_flow_dies_at_unknown_overwrite():
    # If the register is RELOADED before the jump, no constant flows to the jump at all — nothing
    # may be seeded for that path. (Extra candidates would be inert; a MISSING one fail-fasts, so
    # the flow must not invent values out of loads.)
    e, tgt, tgt32 = _fragment_chain_fixture()
    jr_site = e.load + 0x1C
    # patch the addiu t9,t9,32 at +0x18 into a lw t9,0(a0): kills the definite constant
    lw_t9 = (0x23 << 26) | (4 << 21) | (25 << 16) | 0   # lw r25, 0(r4)
    words = bytearray(e.text)
    words[0x18:0x1C] = lw_t9.to_bytes(4, "little")
    e2 = exe_of(bytes(words), e.load)
    sites = emit.unrecovered_jr_targets(e2, funcs=[e.load], entries={e.load}, jt_edges={})
    assert tgt32 not in sites.get(jr_site, set()), \
        "reloaded register must not yield a dispatch target"


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


def test_overlay_jal_shaped_data_only_seeds_real_entries():
    # An overlay is a mixed code/data blob. A data word that decodes as `jal` must not promote an
    # arbitrary instruction in MAIN into the function partition: that splits the resident body at
    # a point with no independent entry contract. A genuine prologue remains seedable.
    a = Asm(0x80010000)
    a.addiu("t0", "zero", 1)
    a.addiu("t1", "zero", 2)
    a.jr("ra")
    a.nop()
    a.addiu("sp", "sp", -16)  # real entry: follows `jr ra; nop`
    a.addiu("t2", "zero", 3)   # non-entry instruction in that function
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    e = exe_of(data)
    real_entry = 0x80010010
    non_entry = 0x80010014
    assert emit.is_func_entry(e, real_entry)
    assert not emit.is_func_entry(e, non_entry)
    fake_jals = struct.pack(
        "<2I",
        (3 << 26) | ((real_entry >> 2) & 0x03FFFFFF),
        (3 << 26) | ((non_entry >> 2) & 0x03FFFFFF),
    )
    with scratch_tempdir("overlay-jal-seeds-") as td:
        open(os.path.join(td, "MIXED.BIN"), "wb").write(fake_jals)
        assert emit.overlay_funcs(e, td) == {real_entry}


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
  uint64_t guest_ticks = 0;       // deterministic instruction time emitted by the shipping translator
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
static void rec_guest_instruction_ticks(Core* c, uint32_t ticks) { c->guest_ticks += ticks; }
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
  printf("lo=%08x\nhi=%08x\ndispatch=%08x\nticks=%llx\n", c.lo, c.hi, g_dispatch,
         static_cast<unsigned long long>(c.guest_ticks));
  return 0;
}
"""


def _have_cxx():
    try:
        subprocess.run(["clang++", "--version"], capture_output=True, check=True)
        return "clang++"
    except Exception:
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
    with scratch_tempdir() as td:
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


def run_module(data, base, entry, regs=None, base_exe=0x80010000, seeds=None,
               overlay_data=None):
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
    with scratch_tempdir() as td:
        # core.h is what the generated TUs include; give them the harness's Core plus the runtime
        # symbols the dispatch TU references.
        prelude = HARNESS.split("__HOOKS__")[0]
        # Every TU includes this, so the harness's definitions have to be inline/static here.
        for sym in ("void gte_hold_pz", "void gte_record_pz", "void gte_hold_src", "void gte_copy_pz",
                    "uint32_t gte_read_data", "void rec_guest_instruction_ticks", "void rec_dispatch(",
                    "uint32_t g_dispatch",
                    "void (*g_dispatch_fn)"):
            prelude = prelude.replace(sym, "inline " + sym)
        core_h = ("#pragma once\n" + prelude
                  + "\ntypedef void (*OverrideFn)(Core*);\n"
                    "inline void rec_dispatch_miss(Core* c, uint32_t a){ rec_dispatch(c, a); }\n")
        open(os.path.join(td, "core.h"), "w").write(core_h)
        overlay_dir = None
        if overlay_data is not None:
            overlay_dir = os.path.join(td, "overlays")
            os.makedirs(overlay_dir)
            open(os.path.join(overlay_dir, "MIXED.BIN"), "wb").write(overlay_data)
        srcs = emit.emit_module(e, td, emit.MAIN_NAMES, seeds or {base}, overlay_dir,
                                shards=1)
        main_cpp = os.path.join(td, "main.cpp")
        open(main_cpp, "w").write(
            '#include "rec_decls.h"\n#include <cstdio>\n#include <cstring>\n'
            "int main(int argc, char** argv){\n"
            "  static Core c; memset(&c, 0, sizeof(c));\n"
            "  for(int i=1;i<argc;i++){ unsigned idx,val; if(sscanf(argv[i],\"r%u=%x\",&idx,&val)==2) c.r[idx]=val;\n"
            "    else { unsigned ad; if(sscanf(argv[i],\"m%x=%x\",&ad,&val)==2) c.mem_w32(ad,val); } }\n"
            f"  gen_func_{entry:08X}(&c);\n"
            '  for(int i=0;i<32;i++) printf("r%d=%08x\\n", i, c.r[i]);\n'
            '  printf("lo=%08x\\nhi=%08x\\ndispatch=%08x\\nticks=%llx\\n", c.lo, c.hi, g_dispatch, '
            'static_cast<unsigned long long>(c.guest_ticks));\n'
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
    assert res["ticks"] == 4, res["ticks"]


def test_exec_overlay_jal_shaped_data_cannot_split_a_resident_delay_slot():
    """Whole-pipeline regression for Toy Story 2's model-table reset corruption.

    A mixed code/data overlay contains a word that decodes as `jal delay_slot`.  That target cannot
    be a function entry: it is the resident loop branch's delay slot and therefore belongs to the
    loop.  Promoting it splits the resident function immediately before the slot, so emit_func sees
    no delay instruction and emits `/* DS */`; the increment then runs only once on fall-through.
    """
    if _skip_if_no_cc():
        return
    base = 0x80010000
    a = Asm(base)
    a.addiu("t0", "zero", 0)
    a.addiu("t1", "zero", 3)
    a.label("loop")
    a.addiu("t0", "t0", 1)
    a.slt("t2", "t0", "t1")
    a.bne("t2", "zero", "loop")
    a.addiu("a1", "a1", 4)        # branch delay slot: must run on all three iterations
    a.addu("v0", "a1", "zero")
    a.jr("ra")
    a.nop()
    data, _ = a.assemble()
    delay_slot = base + 0x14
    fake_jal = (3 << 26) | ((delay_slot >> 2) & 0x03FFFFFF)
    overlay_data = struct.pack("<I", fake_jal)

    result = run_module(data, base, base, regs={"a1": 0}, overlay_data=overlay_data)
    assert result["r"][5] == 12, \
        f"resident branch delay slot ran {result['r'][5] // 4} of 3 iterations"
    assert result["r"][2] == 12, result["r"][2]


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
    # One setup instruction, five four-instruction loop iterations, and jr+nop. A static-body
    # counter would report 7; this proves the shipping emitter counts the path actually executed.
    assert res["ticks"] == 23, res["ticks"]


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


def test_exec_inline_trampoline_table_with_ori_base():
    if _skip_if_no_cc():
        return
    # switch(a0 & 3) via an inline array of fixed-width j+nop trampolines. Unlike the address-table
    # idiom above, jr lands ON the table slot; each slot then jumps into the shared body. Psy-Q forms
    # the base with lui+ori here, not lui+addiu. The branch between the constant construction and the
    # dispatch models Toy Story 2's renderer: it does not write t9 and therefore cannot invalidate the
    # table base. A missing recovery routes the mid-function slot through rec_dispatch instead of
    # keeping it inside this emitted function.
    a = Asm(0x80010000)
    a.lui("t9", 0x8001)
    a.ori("t9", "t9", 0x0028)
    a.bltz("a1", "dispatch_prep")
    a.nop()
    a.label("dispatch_prep")
    a.andi("t1", "a0", 3)
    a.sll("t1", "t1", 3)
    a.addu("t1", "t1", "t9")
    a.jr("t1")
    a.nop()
    a.nop()  # align table to 0x80010028
    a.label("slot0")
    a.j("case0")
    a.nop()
    a.label("slot1")
    a.j("case1")
    a.nop()
    a.label("slot2")
    a.j("case2")
    a.nop()
    a.label("slot3")
    a.j("case3")
    a.nop()
    for index, value in enumerate((11, 22, 33, 44)):
        a.label(f"case{index}")
        a.addiu("v0", "zero", value)
        a.jr("ra")
        a.nop()
    data, _ = a.assemble()
    e = exe_of(data)
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.text_end, 4)}
    recovered = emit.find_jump_tables(e, ins, e.load, e.text_end)
    jr = a.base + 7 * 4
    slots = [a.labels[f"slot{index}"] for index in range(4)]
    assert recovered[jr] == slots
    assert a.labels["case0"] not in recovered[jr], "shared body label is not a table slot"
    for inp, want in enumerate((11, 22, 33, 44)):
        res = run_func(data, a.base, regs={"a0": inp})
        assert res["r"][2] == want, (
            f"inline trampoline switch({inp}) -> {res['r'][2]:#x} != {want}; "
            f"dispatch={res['dispatch']:#x}"
        )


def test_computed_offset_rejects_partial_lui_and_clobbered_low_half():
    # A duplicated tail can contain a jr below the containing function's `lo`. Recovery must not
    # pair that jr with unrelated constants in the containing body. A load kills the prior lui value
    # before the later addiu. The old forward map retained that stale value; newly counting the bare
    # lui as a target too then turned the pair into the false set {0x80010000, 0x8001006A}.
    a = Asm(0x8000FFF0)
    a.jr("v0")
    a.nop()
    a.nop()
    a.nop()
    owner_lo = a.base + 16
    a.lui("v0", 0x8001)
    a.lw("v0", 0x100, "v0")
    a.addiu("v0", "v0", 0x6A)
    a.jr("ra")
    a.nop()
    for _ in range(32):
        a.nop()
    data, _ = a.assemble()
    e = exe_of(data, base=a.base)
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.text_end, 4)}
    assert emit._scan_computed_offset(ins, a.base, 2, owner_lo, e.text_end) is None


def test_computed_offset_rejects_unaligned_masked_targets():
    # A register jump to a non-word-aligned address raises an address error on R3000A; it can never
    # be an emitted C label. Range-only validation admitted base 0x...22 and later walked a
    # halfword-shifted instruction stream until it fell off the image.
    a = Asm(0x80010000)
    a.lui("t9", 0x8001)
    a.ori("t9", "t9", 0x0022)
    a.andi("t1", "a0", 3)
    a.sll("t1", "t1", 3)
    a.addu("t1", "t1", "t9")
    a.jr("t1")
    a.nop()
    for _ in range(12):
        a.nop()
    data, _ = a.assemble()
    e = exe_of(data)
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.text_end, 4)}
    assert a.base + 5 * 4 not in emit.find_jump_tables(e, ins, e.load, e.text_end)


def test_computed_offset_rejects_clobbered_base_constant():
    # The candidate base must still be live at the add. Remembering the last constant ever assigned
    # to t9 after a load overwrote it fabricates a table at the old address.
    a = Asm(0x80010000)
    a.lui("t9", 0x8001)
    a.lw("t9", 0x20, "t9")
    a.andi("t1", "a0", 3)
    a.sll("t1", "t1", 3)
    a.addu("t1", "t1", "t9")
    a.jr("t1")
    a.nop()
    for _ in range(12):
        a.nop()
    data, _ = a.assemble()
    e = exe_of(data)
    ins = {x: decode(x, e.word(x)) for x in range(e.load, e.text_end, 4)}
    assert a.base + 5 * 4 not in emit.find_jump_tables(e, ins, e.load, e.text_end)


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


def test_main_reentry_emits_a_wrapper_body_and_dispatch_case():
    """The saved PC after HookEntryInt's setjmp is inside a function and must be dispatchable."""
    here = os.path.dirname(os.path.abspath(__file__))
    a = Asm()
    a.jr("ra")
    a.nop()
    a.addiu("sp", "sp", -8)       # a natural function starts here
    a.nop()
    reentry = 0x80010010           # deliberately inside that function, after its prologue
    a.addiu("sp", "sp", 8)
    a.jr("ra")
    a.nop()
    text, _ = a.assemble()

    def run(td, seeded):
        exe_path = os.path.join(td, "MAIN.EXE")
        hdr = bytearray(0x800)
        hdr[:8] = b"PS-X EXE"
        struct.pack_into("<II", hdr, 0x10, 0x80010000, 0)
        struct.pack_into("<II", hdr, 0x18, 0x80010000, len(text))
        open(exe_path, "wb").write(bytes(hdr) + text)
        seeds_path = os.path.join(td, "seeds.json")
        values = f'"0x{reentry:08X}"' if seeded else ""
        open(seeds_path, "w").write('{"main_reentry": [' + values + "]}")
        gen = os.path.join(td, "generated")
        os.makedirs(gen)
        env = dict(os.environ, PSXPORT_SHARDS="1", PSXPORT_USE_GHIDRA="0")
        result = subprocess.run([sys.executable, os.path.join(here, "emit.py"), exe_path,
                                 os.path.join(gen, "rec.c"), "--seeds", seeds_path],
                                capture_output=True, text=True, env=env)
        assert result.returncode == 0, f"emit.py failed:\n{result.stdout}\n{result.stderr}"
        return "\n".join(open(os.path.join(gen, name)).read() for name in sorted(os.listdir(gen)))

    with scratch_tempdir("emit-reentry-positive-") as td:
        positive = run(td, True)
        assert f"void func_{reentry:08X}(Core*)" in positive, "main_reentry wrapper was not declared"
        assert f"void gen_func_{reentry:08X}(Core* c)" in positive, "main_reentry body was not emitted"
        assert f"case 0x{reentry & 0x1FFFFFFF:08X}u:" in positive, "main_reentry was not dispatchable"
    with scratch_tempdir("emit-reentry-negative-") as td:
        negative = run(td, False)
        assert f"func_{reentry:08X}" not in negative, "unseeded interior PC was emitted"
        assert f"case 0x{reentry & 0x1FFFFFFF:08X}u:" not in negative, "unseeded interior PC was dispatchable"


def test_jalr_alternate_link_continuation_is_dispatchable():
    """`jalr t2, v1` is a CALL whose return address lands in t2, not $ra — CTR's hand-written GTE
    library calls its helpers this way because $ra holds the caller's inline parameter block, and
    the helper returns through `jr t2`. That `jr t2` routes through the dispatcher, so the link
    value (the jalr's own addr+8, mid-body of the caller) must be a DISPATCHABLE ENTRY even though
    no `jal` names it, no pointer table holds it, and no flow analysis can see the value cross the
    memory-loaded call edge. Measured live: recomp-MISS 0x8006ACE0 from `jr t2` at 0x8006C948
    (docs/issues/0022 in the ctr repo) — the link of `jalr t2, v1` at 0x8006ACD8.

    Discriminators, both directions: a `jalr ra, rs` call returns through the C call stack (its
    `jr ra` is a plain return) and a `jalr zero, rs` is a tail call (the link is discarded), so
    NEITHER may manufacture a continuation entry."""
    here = os.path.dirname(os.path.abspath(__file__))

    def run(td, text, seeds):
        exe_path = os.path.join(td, "MAIN.EXE")
        hdr = bytearray(0x800)
        hdr[:8] = b"PS-X EXE"
        struct.pack_into("<II", hdr, 0x10, 0x80010000, 0)
        struct.pack_into("<II", hdr, 0x18, 0x80010000, len(text))
        open(exe_path, "wb").write(bytes(hdr) + text)
        seeds_path = os.path.join(td, "seeds.json")
        open(seeds_path, "w").write('{"main": [' +
                                    ", ".join(f'"0x{a:08X}"' for a in seeds) + "]}")
        gen = os.path.join(td, "generated")
        os.makedirs(gen)
        env = dict(os.environ, PSXPORT_SHARDS="1", PSXPORT_USE_GHIDRA="0")
        result = subprocess.run([sys.executable, os.path.join(here, "emit.py"), exe_path,
                                 os.path.join(gen, "rec.c"), "--seeds", seeds_path],
                                capture_output=True, text=True, env=env)
        assert result.returncode == 0, f"emit.py failed:\n{result.stdout}\n{result.stderr}"
        return "\n".join(open(os.path.join(gen, name)).read() for name in sorted(os.listdir(gen)))

    # POSITIVE: helper address loaded from the caller's parameter block through $ra (no static
    # constant for the flow analysis to ride), called with the link in t2.
    a = Asm()
    a.lui("ra", 0x8001)             # ra = the inline parameter block pointer
    a.ori("ra", "ra", 0x0200)
    a.lw("v1", 0, "ra")             # v1 = helper address — memory-loaded, invisible to flow
    a.jalr("t2", "v1")              # 0x8001000C: link t2 = 0x80010014
    a.nop()
    a.addiu("v0", "zero", 7)        # 0x80010014: the continuation — never seeded by hand
    a.jr("ra")
    a.nop()
    a.label("callee")               # 0x80010020: reached only through the block pointer
    a.jr("t2")                      # return through the alternate link
    a.nop()
    text, end = a.assemble()
    assert end == 0x80010028, f"layout moved: end={end:#x}"

    with scratch_tempdir("emit-altlink-positive-") as td:
        out = run(td, text, [0x80010000, 0x80010020])
        cont = 0x80010014
        assert f"void func_{cont:08X}(Core*)" in out, \
            "jalr link continuation was not declared as a dispatchable entry"
        assert f"void gen_func_{cont:08X}(Core* c)" in out, \
            "jalr link continuation body was not emitted"
        assert f"case 0x{cont & 0x1FFFFFFF:08X}u:" in out, \
            "jalr link continuation is not routable through the dispatcher"
        assert "c->r[10] = 0x80010014u" in out, \
            "the caller no longer writes the t2 link before dispatching"

    # NEGATIVE: rd=$ra (normal link — the callee's `jr ra` is a plain C return) and rd=$zero
    # (tail call — the link is discarded) must NOT manufacture entries.
    b = Asm()
    b.jalr("ra", "v0")              # 0x80010000: normal-link dynamic call, link = 0x80010008
    b.nop()
    b.addiu("v0", "zero", 7)        # 0x80010008: ordinary fall-through, NOT an entry
    b.jr("ra")
    b.nop()
    b.addiu("v0", "zero", 9)        # 0x80010018: a second caller shape
    b.jalr("zero", "v0")            # tail call: link discarded
    b.nop()
    b.addiu("v0", "zero", 11)
    b.jr("ra")
    b.nop()
    text2, end2 = b.assemble()

    with scratch_tempdir("emit-altlink-negative-") as td:
        out2 = run(td, text2, [0x80010000, 0x80010018])
        assert "void func_80010008" not in out2, \
            "a jalr $ra link continuation was wrongly made dispatchable"
        assert "case 0x00010008u:" not in out2, \
            "a jalr $ra continuation is routable — the discriminator regressed"
        assert "void func_80010020" not in out2, \
            "a jalr $zero tail-call link was wrongly made dispatchable"


def test_reentry_boundary_forgets_ra_for_the_coroutine_proof():
    """A mid-body re-entry seed is a real dispatch edge: control can arrive FRESH there with the
    incoming caller's link in $ra, so a `jr $ra` DOWNSTREAM of a re-entry boundary must not be
    proven a coroutine resume from link-writes that reached it only THROUGH the re-entry's address
    span. Measured live: CTR's GTE macro library — `jalr ra, s6` at 0x8006A534 calls the parameter
    block slot, and the callee fragment 0x8006A8E0's `jr $ra` (0x8006AA94/0x8006AAA0) must be a
    plain return to the incoming link; emitted as a "coroutine resume" dispatch instead, it
    recomp-MISSed on 0x8006A53C (that incoming link, mid-body of the caller fragment)."""
    here = os.path.dirname(os.path.abspath(__file__))
    a = Asm()
    a.lw("v0", 0, "ra")             # 0x80010000: call target read from the parameter block
    a.jalr("ra", "v0")              # 0x80010004: writes ra = 0x8001000C — a 'computed' state
    a.nop()
    a.addiu("v0", "zero", 1)        # 0x8001000C
    a.nop()
    a.nop()
    reentry = 0x80010018            # seeded main_reentry: fresh arrivals have an unknown $ra
    a.addiu("v0", "v0", 1)          # 0x80010018
    a.jr("ra")                      # 0x8001001C: plain return on BOTH arrival paths
    a.nop()
    text, end = a.assemble()
    assert end == 0x80010024, f"layout moved: end={end:#x}"

    with scratch_tempdir("emit-reentry-ra-forget-") as td:
        exe_path = os.path.join(td, "MAIN.EXE")
        hdr = bytearray(0x800)
        hdr[:8] = b"PS-X EXE"
        struct.pack_into("<II", hdr, 0x10, 0x80010000, 0)
        struct.pack_into("<II", hdr, 0x18, 0x80010000, len(text))
        open(exe_path, "wb").write(bytes(hdr) + text)
        seeds_path = os.path.join(td, "seeds.json")
        open(seeds_path, "w").write(
            '{"main": ["0x80010000"], "main_reentry": ["0x%08X"]}' % reentry)
        gen = os.path.join(td, "generated")
        os.makedirs(gen)
        env = dict(os.environ, PSXPORT_SHARDS="1", PSXPORT_USE_GHIDRA="0")
        result = subprocess.run([sys.executable, os.path.join(here, "emit.py"), exe_path,
                                 os.path.join(gen, "rec.c"), "--seeds", seeds_path],
                                capture_output=True, text=True, env=env)
        assert result.returncode == 0, f"emit.py failed:\n{result.stdout}\n{result.stderr}"
        out = "\n".join(open(os.path.join(gen, n)).read() for n in sorted(os.listdir(gen)))
        body = out.split(f"void gen_func_{reentry:08X}(Core* c) {{", 1)[1].split("\n}", 1)[0]
        assert "coroutine resume" not in body, \
            "the jr $ra downstream of a re-entry boundary was still proven a coroutine resume — " \
            "the proof did not forget the state at the fresh-entry edge"
        assert "return;" in body, "the jr $ra downstream of the boundary did not emit a plain return"


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
    with scratch_tempdir() as td:
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


def test_data_overlay_with_jalr_shaped_words_stays_empty():
    """The empty-module guard passes ZERO seeds so a data overlay emits nothing; a seed derivation
    inside emit_module must not defeat it. Measured 2026-08-28: spyro's 512KB OV_18F800 data slice
    (empty module in every emission since Aug 5, `NOT CODE (18% undecodable, 0 jr-ra, 0 prologues)`)
    became 104MB of garbage C when the alternate-link derivation seeded the ~32 garbage `jalr`
    encodings that compressed data decodes to — one per ~4096 random words, so the 4096-word
    negative above cannot see the class. This image plants them deliberately (rd=t2, CTR's link
    register, making the lookalike maximally convincing)."""
    here = os.path.dirname(os.path.abspath(__file__))
    with scratch_tempdir() as td:
        a = Asm()                                   # minimal valid MAIN.EXE
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
        os.makedirs(ovdir)
        data = bytearray(_garbage_image(8192))
        # plant 32 jalr rd=t2, rs=v0 encodings — the alternate-link lookalike — inside the data
        jalr_word = (2 << 21) | (10 << 11) | 9
        for i in range(32):
            struct.pack_into("<I", data, (i * 251) % 8192 * 4, jalr_word)
        open(os.path.join(ovdir, "DATAOVL1.BIN"), "wb").write(bytes(data))
        seeds_path = os.path.join(td, "seeds.json")
        open(seeds_path, "w").write('{"overlay_bases": {"DATAOVL1": "0x8007AA38"}}')
        gen = os.path.join(td, "gen")
        os.makedirs(gen)
        r = subprocess.run([sys.executable, os.path.join(here, "emit.py"),
                            exe_path, os.path.join(gen, "rec.c"),
                            "--seeds", seeds_path, "--overlays", ovdir],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"emit.py failed:\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
        shards = [f for f in os.listdir(gen) if f.startswith("ov_dataovl1_shard_")]
        total = sum(os.path.getsize(os.path.join(gen, f)) for f in shards)
        assert total < 4096, \
            f"data overlay with jalr-shaped words emitted {total} bytes of C across {shards} — " \
            "a seed derivation manufactured entries inside a NOT-CODE module (the spyro " \
            "OV_18F800 104MB regression)"
        disp = open(os.path.join(gen, "ov_dataovl1_disp.c")).read()
        assert "case 0x" not in disp, \
            "a data overlay must have ZERO dispatchable functions even when its bytes " \
            "decode as alternate-link calls"


def test_size_guard_refuses_a_vouched_data_flood():
    """The size guard is the class-wide backstop: whenever data reaches code — by ANY seed path —
    flood-fill and tail duplication inflate the C far beyond the image (measured: spyro 480KB ->
    ~144MB = ~300x; OV_18F800 512KB -> 104MB = ~200x; legit MAIN emissions measure 14-15x). The
    inflation multiplier is garbage-shape-specific (tail duplication re-emits per branch target),
    so this test does not rebuild 200x literally: it vouches 8 seeds into a return-free data image
    — 8 full-span floods of the kind that produced both incidents, measured 17.8x — and lowers the
    guard's knob to 2x so the REFUSAL MECHANISM itself is what is under test: non-zero exit, the
    SIZE GUARD message naming the biggest fragments, and NO shard written."""
    here = os.path.dirname(os.path.abspath(__file__))
    with scratch_tempdir() as td:
        a = Asm()                                   # minimal valid MAIN.EXE
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
        os.makedirs(ovdir)
        nwords = 16384                              # 64KB of data, return-free
        data = bytearray(_garbage_image(nwords))
        base = 0x8007AA38
        # EIGHT explicit vouched seeds in a return-free image: each seed floods to the next entry
        # (nothing terminates the walk), giving full-span garbage bodies of the incident shape.
        seeds = [f"0x{base + (i * nwords // 8) * 4:08X}" for i in range(8)]
        open(os.path.join(ovdir, "FLOODOVL.BIN"), "wb").write(bytes(data))
        seeds_path = os.path.join(td, "seeds.json")
        open(seeds_path, "w").write(
            '{"overlay_bases": {"FLOODOVL": "0x8007AA38"},'
            ' "overlay_seeds": {"FLOODOVL": [' + ", ".join(f'"{s}"' for s in seeds) + ']}}')
        gen = os.path.join(td, "gen")
        os.makedirs(gen)
        env = dict(os.environ, PSXPORT_EMIT_MAX_RATIO="2")
        r = subprocess.run([sys.executable, os.path.join(here, "emit.py"),
                            exe_path, os.path.join(gen, "rec.c"),
                            "--seeds", seeds_path, "--overlays", ovdir],
                           capture_output=True, text=True, env=env)
        combined = r.stdout + r.stderr
        assert "SIZE GUARD" in combined, \
            f"emission of a vouched return-free data flood was not refused (rc={r.returncode}) — " \
            "the size guard did not fire"
        assert r.returncode != 0, "the size guard must refuse (non-zero exit), not just warn"
        assert "Biggest fragments" in combined, \
            "the refusal must NAME the biggest fragments — a bare size is not diagnosable"
        shards = [f for f in os.listdir(gen) if f.startswith("ov_floodovl_shard_")]
        assert not shards, \
            f"the guard refused but shards were already written ({shards}) — refuse BEFORE writing"

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
    with scratch_tempdir() as td:
        r, gen = _run_emit_cli(here, td, ov)
        disp = open(os.path.join(gen, "ov_stage0_disp.c")).read()
        assert "case 0x" not in disp, "seedless noreturn image: the data guard should empty it"
        assert "NOT CODE" in r.stdout + r.stderr
    with scratch_tempdir() as td:
        r, gen = _run_emit_cli(here, td, ov, ov_seeds=["0x8007AA38"])
        disp = open(os.path.join(gen, "ov_stage0_disp.c")).read()
        assert "case 0x" in disp, "an explicit seed vouches for noreturn code — it must recompile"


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


if __name__ == "__main__":
    _main()
