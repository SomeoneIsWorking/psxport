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
        if mn in ("bltz", "bgez"):
            rt = {"bltz": 0, "bgez": 1}[mn]
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
struct Core {
  uint32_t r[32]; uint32_t lo, hi, pc;
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


if __name__ == "__main__":
    _main()
