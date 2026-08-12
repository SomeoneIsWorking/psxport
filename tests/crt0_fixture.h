// crt0_fixture.h — the stock PSY-Q crt0 as an ASSEMBLED SPEC, shared by everything that must prove
// `crt0_scan` (crt0_verify.h) reads a real instruction stream.
//
// ═══ WHY A FIXTURE HEADER AND NOT A BLOB, AND NOT A COPY PER CONSUMER ═══════════════════════════════
// Two consumers need synthetic crt0 bytes: `tests/test_crt0_guest_audit.cpp` (which gates the boot-time
// audit) and `tools/crt0_extract --selftest` (which gates the extractor whose printed constants get
// TYPED into six games' `game_config.cpp`). crt0_extract.cpp's own header comment argues at length that
// the extractor must not carry a second MIPS *decoder*, because two decoders drift and the drift presents
// as the audit blaming a game for the tool's bug. The identical argument applies to the *encoder*: two
// hand-written assemblers would drift, and the drift would present as one of the two gates passing
// against an instruction stream the other does not recognise. So there is exactly one encoder, here.
//
// It is bytes ASSEMBLED FROM A SPEC rather than bytes recorded out of a game, for two reasons: (a) the
// framework must not carry fragments of a commercial executable, and (b) a recorded blob can be wrong in
// the same way the decoder is, while a spec cannot — it says what each instruction IS.
//
// ═══ THE ENCODER IS NOT ASSUMED CORRECT — IT IS PINNED ══════════════════════════════════════════════
// `crt0_fixture_pins()` returns one row per instruction DISASSEMBLED out of a real crt0 on 2026-08-12
// (scratch/crt0_dump.py over Tomba!2 MAIN.EXE and SLUS_005.61), each row being "the word this encoder
// produced" next to "the word the real executable contains". EVERY CONSUMER MUST CHECK THOSE ROWS BEFORE
// USING THE FIXTURE, and must fail if any row differs — otherwise the cases below pass against a stream
// no PSX would execute. Both current consumers do it as their first case.
//
// ═══ THE CORPUS THIS DESCRIBES ══════════════════════════════════════════════════════════════════════
// The prologue template is the stock PSY-Q crt0, verified instruction-for-instruction identical across
// FIVE executables (Tomba!2, Spyro, Spider-Man, Vagrant Story, Mega Man X4); the per-title entry points
// and biases are tabulated in crt0_verify.h. Two shapes cover the measured variation, and both are
// provided below as named fixtures so no consumer invents its own addresses:
//   * `crt0_fixture_psyq_tomba2()` — the common shape: direct stack-top load, `addi v0,v0,-8` bias, BOTH
//     absolute heap globals.
//   * `crt0_fixture_psyq_mmx4()`   — the outlier: X4's indirect stack-top addressing, NO bias instruction
//     at all (a measured 0, not an unknown), and NEITHER heap global.
//
// ═══ WHY IT LIVES IN tests/ AND NOT BESIDE crt0_verify.h ════════════════════════════════════════════
// It was written into `runtime/recomp/` first, on the argument that the corpus knowledge it encodes is
// crt0_verify.h's. That argument is wrong, and the tree said so mechanically: the move took the guest
// literal count in framework code from 399 to 429 and turned BOTH literal gates RED
// (`tools/lint/game_literals.py`, `tests/test_no_game_address_literals`). They were right to fire. The
// addresses below are five shipping games' REAL crt0 groups, and `runtime/` is exactly where a real guest
// address may not be a literal — which is why crt0_verify.h carries the same per-title table as a
// COMMENT. This file needs them as live code, so it belongs where fixtures live and the scanners already
// skip: `tests/`. It is compiled into NOTHING — `libpsxport` does not include it.
//
// `tools/crt0_extract.cpp` therefore includes it from `../tests/`. That is deliberate and not an
// accident of layout: crt0_extract's `--selftest` IS a test, it is the only thing gating that tool's
// refusals, and one shared encoder in the fixture directory beats a second copy inside a scanned root.
#pragma once
#include <stdint.h>
#include <vector>

// ── a minimal R3000A assembler ──────────────────────────────────────────────────────────────────────
enum Crt0FixtureReg {
  CF_ZERO = 0, CF_AT = 1, CF_V0 = 2, CF_V1 = 3, CF_A0 = 4, CF_A1 = 5, CF_T0 = 8,
  CF_GP = 28, CF_SP = 29, CF_FP = 30, CF_RA = 31
};
static inline uint32_t cf_i16(int32_t v) { return (uint32_t)v & 0xFFFFu; }
static inline uint32_t cf_lui  (int rt, uint32_t hi)        { return 0x3C000000u | (uint32_t)rt << 16 | (hi & 0xFFFF); }
static inline uint32_t cf_addiu(int rt, int rs, int32_t im)  { return 0x24000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | cf_i16(im); }
static inline uint32_t cf_addi (int rt, int rs, int32_t im)  { return 0x20000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | cf_i16(im); }
static inline uint32_t cf_lw   (int rt, int rs, int32_t im)  { return 0x8C000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | cf_i16(im); }
static inline uint32_t cf_sw   (int rt, int rs, int32_t im)  { return 0xAC000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | cf_i16(im); }
static inline uint32_t cf_bne  (int rs, int rt, int32_t off) { return 0x14000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | cf_i16(off); }
static inline uint32_t cf_spec (int rd, int rs, int rt, uint32_t sa, uint32_t fn) {
  return (uint32_t)rs << 21 | (uint32_t)rt << 16 | (uint32_t)rd << 11 | sa << 6 | fn;
}
static inline uint32_t cf_sltu(int rd, int rs, int rt) { return cf_spec(rd, rs, rt, 0, 0x2B); }
static inline uint32_t cf_subu(int rd, int rs, int rt) { return cf_spec(rd, rs, rt, 0, 0x23); }
static inline uint32_t cf_or  (int rd, int rs, int rt) { return cf_spec(rd, rs, rt, 0, 0x25); }
static inline uint32_t cf_addu(int rd, int rs, int rt) { return cf_spec(rd, rs, rt, 0, 0x21); }
static inline uint32_t cf_sll (int rd, int rt, uint32_t sa) { return cf_spec(rd, 0, rt, sa, 0x00); }
static inline uint32_t cf_srl (int rd, int rt, uint32_t sa) { return cf_spec(rd, 0, rt, sa, 0x02); }
static inline uint32_t cf_jal (uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x3FFFFFFu); }

// `lui rX,hi / addiu rX,rX,lo` for an absolute 32-bit address (the standard %hi/%lo pair: the addiu's
// immediate is SIGN-extended, so hi is adjusted when lo's top bit is set).
static inline uint32_t CF_HI(uint32_t a) { return ((a >> 16) + ((a & 0x8000u) ? 1u : 0u)) & 0xFFFFu; }
static inline int32_t  CF_LO(uint32_t a) { return (int32_t)(int16_t)(uint16_t)(a & 0xFFFFu); }

// ── the pins: this encoder's output vs bytes disassembled from a real crt0 ───────────────────────────
struct Crt0PinRow { const char* what; uint32_t got, want; };

// The FLOOR lives here, next to the rows it counts, because a `for` over the rows passes while proving
// nothing if the list is empty or has been whittled down. Both consumers assert the count against this
// before their loop, so "0 mismatches" cannot be a report of never having looked — and there is ONE
// number to raise when a row is added, not one per consumer to forget.
static const int CRT0_FIXTURE_PIN_FLOOR = 21;   // 18 measured instructions + 3 %hi/%lo round trips

// Every row is one instruction read out of a real executable. A consumer that uses the fixture without
// checking these is testing a decoder against an encoder, with no third party.
static inline std::vector<Crt0PinRow> crt0_fixture_pins(void) {
  return {
    {"0x800896E0 lui   v0,0x800C",        cf_lui(CF_V0, 0x800C),      0x3C02800Cu},
    {"0x800896E4 addiu v0,v0,-7976",      cf_addiu(CF_V0, CF_V0, -7976), 0x2442E0D8u},
    {"0x800896F0 sw    zero,0(v0)",       cf_sw(CF_ZERO, CF_V0, 0),   0xAC400000u},
    {"0x800896F8 sltu  at,v0,v1",         cf_sltu(CF_AT, CF_V0, CF_V1), 0x0043082Bu},
    {"0x800896FC bne   at,zero,-4",       cf_bne(CF_AT, CF_ZERO, -4), 0x1420FFFCu},
    {"0x80089708 lw    v0,16264(v0)",     cf_lw(CF_V0, CF_V0, 16264), 0x8C423F88u},
    {"0x80089710 addi  v0,v0,-8",         cf_addi(CF_V0, CF_V0, -8),  0x2042FFF8u},
    {"0x80089714 lui   t0,0x8000",        cf_lui(CF_T0, 0x8000),      0x3C088000u},
    {"0x80089718 or    sp,v0,t0",         cf_or(CF_SP, CF_V0, CF_T0), 0x0048E825u},
    {"0x80089724 sll   a0,a0,3",          cf_sll(CF_A0, CF_A0, 3),    0x000420C0u},
    {"0x80089728 srl   a0,a0,3",          cf_srl(CF_A0, CF_A0, 3),    0x000420C2u},
    {"0x80089738 subu  a1,v0,v1",         cf_subu(CF_A1, CF_V0, CF_V1), 0x00432823u},
    {"0x8008973C subu  a1,a1,a0",         cf_subu(CF_A1, CF_A1, CF_A0), 0x00A42823u},
    {"0x80089744 sw    a1,-16648(at)",    cf_sw(CF_A1, CF_AT, -16648), 0xAC25BEF8u},
    {"0x80089764 addu  fp,sp,zero",       cf_addu(CF_FP, CF_SP, CF_ZERO), 0x03A0F021u},
    {"0x80089768 jal   0x80089860",       cf_jal(0x80089860u),        0x0C022618u},
    {"0x800DAECC addu  a0,a0,v0 (X4)",    cf_addu(CF_A0, CF_A0, CF_V0), 0x00822021u},
    {"0x800DAED0 lw    v0,0(a0)  (X4)",   cf_lw(CF_V0, CF_A0, 0),     0x8C820000u},
    // …and the %hi/%lo pair really reconstructs the address, including the sign-extension carry.
    {"%hi/%lo rebuilds 0x800BE0D8",  (CF_HI(0x800BE0D8u) << 16) + (uint32_t)CF_LO(0x800BE0D8u), 0x800BE0D8u},
    {"%hi/%lo rebuilds 0x800A3F88",  (CF_HI(0x800A3F88u) << 16) + (uint32_t)CF_LO(0x800A3F88u), 0x800A3F88u},
    {"%hi/%lo rebuilds 0x80106228",  (CF_HI(0x80106228u) << 16) + (uint32_t)CF_LO(0x80106228u), 0x80106228u},
  };
}

// ── the template ────────────────────────────────────────────────────────────────────────────────────
// `bias` is emitted as an `addi v0,v0,bias` ONLY when nonzero — which is exactly the difference between
// the four ports that bias and Mega Man X4, which has no such instruction. `heapSizePtr`/`heapBasePtr`
// at 0 emit NO store (X4 emits neither). `indirectStackTop` emits X4's own addressing for the stack-top
// load (`addiu v0,zero,4 / lui a0,hi / addiu a0,a0,lo / addu a0,a0,v0 / lw v0,0(a0)`) instead of the
// direct `lui v0 / lw v0,lo(v0)`, so the decoder is exercised on BOTH forms in the corpus.
struct Crt0Template {
  uint32_t entry, bssLo, bssHi, stackTopBase, stackTopBase2, heapBase, gp, libcInit;
  int32_t  bias = -8;
  uint32_t heapSizePtr = 0, heapBasePtr = 0;   // 0 = this crt0 has no such global
  uint32_t raSave = 0;                         // the one absolute store X4 does have
  bool     indirectStackTop = false;
};

// The BIOS A(39h) InitHeap thunk the crt0's `jal` lands on, in all five measured executables:
// addiu t2,zero,0xA0 / jr t2 / addiu t1,zero,0x39. `crt0_scan` recognises libcInit by these three words.
static const uint32_t CRT0_INITHEAP_THUNK[3] = {0x240A00A0u, 0x01400008u, 0x24090039u};

// crt0_fixture_words — the prologue, as words starting at `t.entry`.
static inline std::vector<uint32_t> crt0_fixture_words(const Crt0Template& t) {
  std::vector<uint32_t> c;
  // .bss clear loop: v0 = bssLo, v1 = bssHi, then `sw zero,0(v0); addiu v0,v0,4; sltu at,v0,v1; bne`.
  c.push_back(cf_lui(CF_V0, CF_HI(t.bssLo)));   c.push_back(cf_addiu(CF_V0, CF_V0, CF_LO(t.bssLo)));
  c.push_back(cf_lui(CF_V1, CF_HI(t.bssHi)));   c.push_back(cf_addiu(CF_V1, CF_V1, CF_LO(t.bssHi)));
  c.push_back(cf_sw(CF_ZERO, CF_V0, 0));
  c.push_back(cf_addiu(CF_V0, CF_V0, 4));
  c.push_back(cf_sltu(CF_AT, CF_V0, CF_V1));
  c.push_back(cf_bne(CF_AT, CF_ZERO, -4));
  c.push_back(0);                                             // delay slot
  // sp = (mem[stackTopBase] + bias) | KSEG0
  if (t.indirectStackTop) {
    c.push_back(cf_addiu(CF_V0, CF_ZERO, 4));
    c.push_back(cf_lui(CF_A0, CF_HI(t.stackTopBase - 4)));
    c.push_back(cf_addiu(CF_A0, CF_A0, CF_LO(t.stackTopBase - 4)));
    c.push_back(cf_addu(CF_A0, CF_A0, CF_V0));
    c.push_back(cf_lw(CF_V0, CF_A0, 0));
  } else {
    c.push_back(cf_lui(CF_V0, CF_HI(t.stackTopBase)));
    c.push_back(cf_lw(CF_V0, CF_V0, CF_LO(t.stackTopBase)));
    c.push_back(0);                                           // load delay
  }
  if (t.bias) c.push_back(cf_addi(CF_V0, CF_V0, t.bias));
  c.push_back(cf_lui(CF_T0, 0x8000));
  c.push_back(cf_or(CF_SP, CF_V0, CF_T0));
  // a0 = heapBase & 0x1FFFFFFF, via sll 3 / srl 3
  c.push_back(cf_lui(CF_A0, CF_HI(t.heapBase)));  c.push_back(cf_addiu(CF_A0, CF_A0, CF_LO(t.heapBase)));
  c.push_back(cf_sll(CF_A0, CF_A0, 3));  c.push_back(cf_srl(CF_A0, CF_A0, 3));
  // a1 = (v0 - mem[stackTopBase2]) - a0
  c.push_back(cf_lui(CF_V1, CF_HI(t.stackTopBase2)));
  c.push_back(cf_lw(CF_V1, CF_V1, CF_LO(t.stackTopBase2)));
  c.push_back(0);
  c.push_back(cf_subu(CF_A1, CF_V0, CF_V1));
  c.push_back(cf_subu(CF_A1, CF_A1, CF_A0));
  if (t.heapSizePtr) {
    c.push_back(cf_lui(CF_AT, CF_HI(t.heapSizePtr)));
    c.push_back(cf_sw(CF_A1, CF_AT, CF_LO(t.heapSizePtr)));
  }
  c.push_back(cf_or(CF_A0, CF_A0, CF_T0));
  if (t.heapBasePtr) {
    c.push_back(cf_lui(CF_AT, CF_HI(t.heapBasePtr)));
    c.push_back(cf_sw(CF_A0, CF_AT, CF_LO(t.heapBasePtr)));
  }
  if (t.raSave) {
    c.push_back(cf_lui(CF_AT, CF_HI(t.raSave)));
    c.push_back(cf_sw(CF_RA, CF_AT, CF_LO(t.raSave)));
  }
  c.push_back(cf_lui(CF_GP, CF_HI(t.gp)));  c.push_back(cf_addiu(CF_GP, CF_GP, CF_LO(t.gp)));
  c.push_back(cf_addu(CF_FP, CF_SP, CF_ZERO));
  c.push_back(cf_jal(t.libcInit));
  c.push_back(cf_addi(CF_A0, CF_A0, 4));                      // the delay slot IS the +4
  return c;
}

// crt0_fixture_emit — write the prologue AND the InitHeap thunk it calls through `store(addr, word)`.
// Templated on the sink so the audit test can write into a std::map standing in for guest RAM while
// crt0_extract's selftest writes into a PS-X EXE image, WITHOUT a second copy of the emitter.
template <class Store>
static inline void crt0_fixture_emit(const Crt0Template& t, Store store) {
  const std::vector<uint32_t> c = crt0_fixture_words(t);
  for (size_t i = 0; i < c.size(); i++) store(t.entry + 4u * (uint32_t)i, c[i]);
  for (int i = 0; i < 3; i++) store(t.libcInit + 4u * (uint32_t)i, CRT0_INITHEAP_THUNK[i]);
}

// ── the two measured shapes ─────────────────────────────────────────────────────────────────────────
// Addresses are the MEASURED groups of the two real ports (Tomba2Engine and megamanx4
// game/core/game_config.cpp), so both fixtures describe real crt0s rather than invented ones. Keep them
// here rather than per-consumer: a consumer inventing its own addresses is a consumer whose "pass" says
// nothing about any shipping game.
static const uint32_t CRT0_FIXTURE_TOMBA2_ENTRY = 0x800896E0u;
static const uint32_t CRT0_FIXTURE_MMX4_ENTRY   = 0x800DAE8Cu;

static inline Crt0Template crt0_fixture_psyq_tomba2(void) {
  return Crt0Template{CRT0_FIXTURE_TOMBA2_ENTRY, 0x800BE0D8u, 0x80106228u, 0x800A3F88u, 0x800A3F8Cu,
                      0x80106228u, 0x800BE0D4u, 0x80089860u, -8, 0x800ABEF8u, 0x800ABEF4u,
                      0x800BE0D8u, false};
}
static inline Crt0Template crt0_fixture_psyq_mmx4(void) {
  return Crt0Template{CRT0_FIXTURE_MMX4_ENTRY, 0x8012F418u, 0x80175F38u, 0x800DAF3Cu, 0x8011CB74u,
                      0x80175F38u, 0x8012F418u, 0x800EDCDCu, 0, 0, 0, 0x8012F418u, true};
}
