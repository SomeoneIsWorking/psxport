// test_crt0_guest_audit.cpp — THE GATE RULE, for the crt0 boot group.
//
// WHAT IT GATES. Every constant in a game's `GameConfig` crt0 group is a value somebody measured out of
// the executable and typed into `game/core/game_config.cpp`. Nothing compared the typed copy to the
// measurement, which is the defect shape found independently in four of five ports here. `crt0_audit`
// (runtime/recomp/crt0_verify.h) closes it by re-deriving the group from the guest's OWN instruction
// stream at boot and refusing a confirmed disagreement. This file gates the auditor in BOTH directions:
// an agreeing config must pass and say how many fields it checked, and a disagreeing one must REFUSE and
// name the field — including the exact disagreement the pre-fix framework shipped (a hardcoded -8 stack
// bias against a guest that has no bias instruction).
//
// WHY THE GUEST BYTES ARE ASSEMBLED HERE RATHER THAN COPIED FROM A GAME. Two reasons, one of them
// technical: (a) the framework must not carry bytes lifted out of a commercial executable, and (b) an
// assembler in the test makes the fixture a SPEC the decoder is checked against, instead of one recorded
// blob that could be wrong in the same way the decoder is.
//
// THE ASSEMBLER IS NOT ASSUMED CORRECT — IT IS PINNED TO MEASURED BYTES. Each encoder below carries the
// exact word it must produce for one instruction disassembled out of a real crt0 on 2026-08-12
// (scratch/crt0_dump.py over Tomba!2 MAIN.EXE / SLUS_005.61), and `test_assembler_matches_measured_bytes`
// asserts every one of them BEFORE any audit case runs. If the assembler drifts, that test fails first
// and the audit cases below cannot pass vacuously against a fixture nobody recognises.
//
// The template itself is the stock PSY-Q crt0, verified instruction-for-instruction identical across
// FIVE executables (Tomba!2, Spyro, Spider-Man, Vagrant Story, Mega Man X4) — the table in
// crt0_verify.h records the five entry points and their measured biases.
//
// Hermetic: a std::map stands in for guest RAM, lucent's sink is in-process. No Core, no disc, no GPU.
#include "testutil.h"

#include "crt0_verify.h"

#include <lucent/log.h>

#include <map>
#include <string>
#include <vector>

// ── the capture sink ────────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> g_lines;
static int g_errors = 0, g_warns = 0;
static void capture_start(void) {
  g_lines.clear(); g_errors = g_warns = 0;
  lucent::set_sink([](lucent::Level lv, std::string_view line) {
    g_lines.emplace_back(line);
    if (lv == lucent::Level::Error) g_errors++;
    if (lv == lucent::Level::Warn)  g_warns++;
  });
}
static void capture_stop(void) { lucent::set_sink(nullptr); }
static void dump_capture(const char* what) {
  fprintf(stderr, "  [%s] captured %zu line(s), %d error / %d warn\n", what, g_lines.size(), g_errors, g_warns);
  for (const std::string& l : g_lines) fprintf(stderr, "  [%s] | %s\n", what, l.c_str());
}
static bool any_line_has(const char* needle) {
  for (const std::string& l : g_lines) if (l.find(needle) != std::string::npos) return true;
  return false;
}

// ── a minimal R3000A assembler ──────────────────────────────────────────────────────────────────────
enum { ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, T0 = 8, GP = 28, SP = 29, FP = 30, RA = 31 };
static uint32_t I16(int32_t v) { return (uint32_t)v & 0xFFFFu; }
static uint32_t A_lui  (int rt, uint32_t hi)             { return 0x3C000000u | (uint32_t)rt << 16 | (hi & 0xFFFF); }
static uint32_t A_addiu(int rt, int rs, int32_t im)      { return 0x24000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | I16(im); }
static uint32_t A_addi (int rt, int rs, int32_t im)      { return 0x20000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | I16(im); }
static uint32_t A_lw   (int rt, int rs, int32_t im)      { return 0x8C000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | I16(im); }
static uint32_t A_sw   (int rt, int rs, int32_t im)      { return 0xAC000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | I16(im); }
static uint32_t A_bne  (int rs, int rt, int32_t off)     { return 0x14000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | I16(off); }
static uint32_t A_spec (int rd, int rs, int rt, uint32_t sa, uint32_t fn) {
  return (uint32_t)rs << 21 | (uint32_t)rt << 16 | (uint32_t)rd << 11 | sa << 6 | fn;
}
static uint32_t A_sltu(int rd, int rs, int rt) { return A_spec(rd, rs, rt, 0, 0x2B); }
static uint32_t A_subu(int rd, int rs, int rt) { return A_spec(rd, rs, rt, 0, 0x23); }
static uint32_t A_or  (int rd, int rs, int rt) { return A_spec(rd, rs, rt, 0, 0x25); }
static uint32_t A_addu(int rd, int rs, int rt) { return A_spec(rd, rs, rt, 0, 0x21); }
static uint32_t A_sll (int rd, int rt, uint32_t sa) { return A_spec(rd, 0, rt, sa, 0x00); }
static uint32_t A_srl (int rd, int rt, uint32_t sa) { return A_spec(rd, 0, rt, sa, 0x02); }
static uint32_t A_jal (uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x3FFFFFFu); }

// `lui rX,hi / addiu rX,rX,lo` for an absolute 32-bit address (the standard %hi/%lo pair: the addiu's
// immediate is SIGN-extended, so hi is adjusted when lo's top bit is set).
static uint32_t HI(uint32_t a) { return ((a >> 16) + ((a & 0x8000u) ? 1u : 0u)) & 0xFFFFu; }
static int32_t  LO(uint32_t a) { return (int32_t)(int16_t)(uint16_t)(a & 0xFFFFu); }

// ── the memory model ────────────────────────────────────────────────────────────────────────────────
struct FakeRam {
  std::map<uint32_t, uint32_t> w;
  uint32_t read(uint32_t a) const { auto it = w.find(a); return it == w.end() ? 0u : it->second; }
};

// The stock PSY-Q crt0. `bias` is emitted as an `addi v0,v0,bias` ONLY when nonzero — which is exactly
// the difference between the four ports that bias and Mega Man X4, which has no such instruction.
// `stores` emits the two absolute heap globals; X4 emits NEITHER.
// `indirectStackTop` emits X4's own addressing for the stack-top load (`addiu v0,zero,4 / lui a0,hi /
// addiu a0,a0,lo / addu a0,a0,v0 / lw v0,0(a0)`) instead of the direct `lui v0 / lw v0,lo(v0)` — so the
// decoder is exercised on BOTH forms found in the corpus rather than only the common one.
struct Crt0Template {
  uint32_t entry, bssLo, bssHi, stackTopBase, stackTopBase2, heapBase, gp, libcInit;
  int32_t  bias = -8;
  uint32_t heapSizePtr = 0, heapBasePtr = 0;   // 0 = this crt0 has no such global
  uint32_t raSave = 0;                         // the one absolute store X4 does have
  bool     indirectStackTop = false;
};

static void emit_crt0(FakeRam& ram, const Crt0Template& t) {
  std::vector<uint32_t> c;
  // .bss clear loop: v0 = bssLo, v1 = bssHi, then `sw zero,0(v0); addiu v0,v0,4; sltu at,v0,v1; bne`.
  c.push_back(A_lui(V0, HI(t.bssLo)));   c.push_back(A_addiu(V0, V0, LO(t.bssLo)));
  c.push_back(A_lui(V1, HI(t.bssHi)));   c.push_back(A_addiu(V1, V1, LO(t.bssHi)));
  c.push_back(A_sw(ZERO, V0, 0));
  c.push_back(A_addiu(V0, V0, 4));
  c.push_back(A_sltu(AT, V0, V1));
  c.push_back(A_bne(AT, ZERO, -4));
  c.push_back(0);                                             // delay slot
  // sp = (mem[stackTopBase] + bias) | KSEG0
  if (t.indirectStackTop) {
    c.push_back(A_addiu(V0, ZERO, 4));
    c.push_back(A_lui(A0, HI(t.stackTopBase - 4)));  c.push_back(A_addiu(A0, A0, LO(t.stackTopBase - 4)));
    c.push_back(A_addu(A0, A0, V0));
    c.push_back(A_lw(V0, A0, 0));
  } else {
    c.push_back(A_lui(V0, HI(t.stackTopBase)));  c.push_back(A_lw(V0, V0, LO(t.stackTopBase)));
    c.push_back(0);                                           // load delay
  }
  if (t.bias) c.push_back(A_addi(V0, V0, t.bias));
  c.push_back(A_lui(T0, 0x8000));
  c.push_back(A_or(SP, V0, T0));
  // a0 = heapBase & 0x1FFFFFFF, via sll 3 / srl 3
  c.push_back(A_lui(A0, HI(t.heapBase)));  c.push_back(A_addiu(A0, A0, LO(t.heapBase)));
  c.push_back(A_sll(A0, A0, 3));  c.push_back(A_srl(A0, A0, 3));
  // a1 = (v0 - mem[stackTopBase2]) - a0
  c.push_back(A_lui(V1, HI(t.stackTopBase2)));  c.push_back(A_lw(V1, V1, LO(t.stackTopBase2)));
  c.push_back(0);
  c.push_back(A_subu(A1, V0, V1));
  c.push_back(A_subu(A1, A1, A0));
  if (t.heapSizePtr) { c.push_back(A_lui(AT, HI(t.heapSizePtr))); c.push_back(A_sw(A1, AT, LO(t.heapSizePtr))); }
  c.push_back(A_or(A0, A0, T0));
  if (t.heapBasePtr) { c.push_back(A_lui(AT, HI(t.heapBasePtr))); c.push_back(A_sw(A0, AT, LO(t.heapBasePtr))); }
  if (t.raSave)      { c.push_back(A_lui(AT, HI(t.raSave)));      c.push_back(A_sw(RA, AT, LO(t.raSave))); }
  c.push_back(A_lui(GP, HI(t.gp)));  c.push_back(A_addiu(GP, GP, LO(t.gp)));
  c.push_back(A_addu(FP, SP, ZERO));
  c.push_back(A_jal(t.libcInit));
  c.push_back(A_addi(A0, A0, 4));                             // the delay slot IS the +4
  for (size_t i = 0; i < c.size(); i++) ram.w[t.entry + 4u * (uint32_t)i] = c[i];
  // the BIOS A(39h) InitHeap thunk the jal lands on
  ram.w[t.libcInit + 0] = 0x240A00A0u;   // addiu t2,zero,0xA0
  ram.w[t.libcInit + 4] = 0x01400008u;   // jr t2
  ram.w[t.libcInit + 8] = 0x24090039u;   // addiu t1,zero,0x39
}

// ── the two shapes, as GameConfigs ──────────────────────────────────────────────────────────────────
// Addresses are the MEASURED groups of the two real ports (Tomba2Engine and megamanx4
// game/core/game_config.cpp), so the fixtures describe real crt0s rather than invented ones.
static const uint32_t T2_CRT0 = 0x800896E0u;
static GameConfig t2_cfg(void) {
  GameConfig c{};
  c.bssZeroLo = 0x800BE0D8u; c.bssZeroHi = 0x80106228u;
  c.stackTopBase = 0x800A3F88u; c.stackTopBase2 = 0x800A3F8Cu;
  c.heapBase = 0x80106228u;
  c.heapSizePtr = 0x800ABEF8u; c.heapBasePtr = 0x800ABEF4u;
  c.gp = 0x800BE0D4u; c.libcInit = 0x80089860u; c.crt0 = T2_CRT0;
  c.stackBias = {1u, -8};
  return c;
}
static Crt0Template t2_guest(void) {
  return Crt0Template{T2_CRT0, 0x800BE0D8u, 0x80106228u, 0x800A3F88u, 0x800A3F8Cu, 0x80106228u,
                      0x800BE0D4u, 0x80089860u, -8, 0x800ABEF8u, 0x800ABEF4u, 0x800BE0D8u, false};
}
static const uint32_t X4_CRT0 = 0x800DAE8Cu;
static GameConfig x4_cfg(void) {
  GameConfig c{};
  c.bssZeroLo = 0x8012F418u; c.bssZeroHi = 0x80175F38u;
  c.stackTopBase = 0x800DAF3Cu; c.stackTopBase2 = 0x8011CB74u;
  c.heapBase = 0x80175F38u;
  c.heapSizePtr = 0; c.heapBasePtr = 0;                      // ABSENT — this crt0 has no such global
  c.gp = 0x8012F418u; c.libcInit = 0x800EDCDCu; c.crt0 = X4_CRT0;
  c.stackBias = {1u, 0};
  return c;
}
static Crt0Template x4_guest(void) {
  return Crt0Template{X4_CRT0, 0x8012F418u, 0x80175F38u, 0x800DAF3Cu, 0x8011CB74u, 0x80175F38u,
                      0x8012F418u, 0x800EDCDCu, 0, 0, 0, 0x8012F418u, true};
}

// A plan is needed only so the audit can print the applied values next to the verified ones.
static Crt0Plan plan_for(const GameConfig& cfg, uint32_t stackWord, uint32_t reserveWord) {
  lucent::set_sink([](lucent::Level, std::string_view) {});      // the plan's own log is not under test
  Crt0Plan p = crt0_plan(&cfg, stackWord, reserveWord, "fixture");
  lucent::set_sink(nullptr);
  return p;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 0: the assembler is pinned to bytes disassembled from a real crt0. Runs FIRST: if these fail,
// every fixture below describes an instruction stream no PSX would execute, and the audit cases would be
// passing against nothing.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
static void test_assembler_matches_measured_bytes(void) {
  CHECK_EQ(A_lui(V0, 0x800C),        0x3C02800Cu);   // 0x800896E0 lui   v0,0x800C
  CHECK_EQ(A_addiu(V0, V0, -7976),   0x2442E0D8u);   // 0x800896E4 addiu v0,v0,-7976
  CHECK_EQ(A_sw(ZERO, V0, 0),        0xAC400000u);   // 0x800896F0 sw    zero,0(v0)
  CHECK_EQ(A_sltu(AT, V0, V1),       0x0043082Bu);   // 0x800896F8 sltu  at,v0,v1
  CHECK_EQ(A_bne(AT, ZERO, -4),      0x1420FFFCu);   // 0x800896FC bne   at,zero,-4
  CHECK_EQ(A_lw(V0, V0, 16264),      0x8C423F88u);   // 0x80089708 lw    v0,16264(v0)
  CHECK_EQ(A_addi(V0, V0, -8),       0x2042FFF8u);   // 0x80089710 addi  v0,v0,-8
  CHECK_EQ(A_lui(T0, 0x8000),        0x3C088000u);   // 0x80089714 lui   t0,0x8000
  CHECK_EQ(A_or(SP, V0, T0),         0x0048E825u);   // 0x80089718 or    sp,v0,t0
  CHECK_EQ(A_sll(A0, A0, 3),         0x000420C0u);   // 0x80089724 sll   a0,a0,3
  CHECK_EQ(A_srl(A0, A0, 3),         0x000420C2u);   // 0x80089728 srl   a0,a0,3
  CHECK_EQ(A_subu(A1, V0, V1),       0x00432823u);   // 0x80089738 subu  a1,v0,v1
  CHECK_EQ(A_subu(A1, A1, A0),       0x00A42823u);   // 0x8008973C subu  a1,a1,a0
  CHECK_EQ(A_sw(A1, AT, -16648),     0xAC25BEF8u);   // 0x80089744 sw    a1,-16648(at)
  CHECK_EQ(A_addu(FP, SP, ZERO),     0x03A0F021u);   // 0x80089764 addu  fp,sp,zero
  CHECK_EQ(A_jal(0x80089860u),       0x0C022618u);   // 0x80089768 jal   0x80089860
  CHECK_EQ(A_addu(A0, A0, V0),       0x00822021u);   // 0x800DAECC addu  a0,a0,v0   (X4's form)
  CHECK_EQ(A_lw(V0, A0, 0),          0x8C820000u);   // 0x800DAED0 lw    v0,0(a0)   (X4's form)
  // And the %hi/%lo pair really reconstructs the address, including the sign-extension carry.
  CHECK_EQ((HI(0x800BE0D8u) << 16) + (uint32_t)LO(0x800BE0D8u), 0x800BE0D8u);
  CHECK_EQ((HI(0x800A3F88u) << 16) + (uint32_t)LO(0x800A3F88u), 0x800A3F88u);
  CHECK_EQ((HI(0x80106228u) << 16) + (uint32_t)LO(0x80106228u), 0x80106228u);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 1: an AGREEING config passes, and says how many fields it checked. The count is asserted because
// "0 DISAGREE" over 0 fields checked is the exact false pass this whole file exists to prevent.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
static void test_agreeing_config_passes_with_a_denominator(void) {
  FakeRam ram; emit_crt0(ram, t2_guest());
  const GameConfig cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-t2");
  capture_stop();
  if (!ok) dump_capture("t2");
  CHECK(ok);
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("10 field(s) AGREE, 0 DISAGREE, 0 unresolved"));
  CHECK(any_line_has("libcInit IS the A(39h) InitHeap thunk"));
  CHECK(any_line_has("a1 IS live at the guest's own jal"));
  CHECK(any_line_has("delay slot is `addi a0,a0,4`"));
}

// The other shape: no bias instruction, X4's indirect stack-top load, and NEITHER heap global.
static void test_agreeing_x4_shape_passes(void) {
  FakeRam ram; emit_crt0(ram, x4_guest());
  const GameConfig cfg = x4_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-x4");
  capture_stop();
  if (!ok) dump_capture("x4");
  CHECK(ok);
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("10 field(s) AGREE, 0 DISAGREE, 0 unresolved"));
  CHECK(any_line_has("a1 IS live at the guest's own jal"));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 2: THE PRE-FIX FRAMEWORK'S OWN BUG. The guest has no bias instruction; the config claims -8 (what
// native_boot.cpp hardcoded for every consumer). The audit must REFUSE and name `stackBias`.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
static void test_wrong_bias_is_refused_and_named(void) {
  FakeRam ram; emit_crt0(ram, x4_guest());
  GameConfig cfg = x4_cfg();
  cfg.stackBias = {1u, -8};                              // the value the old framework applied to all
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-badbias");
  capture_stop();
  dump_capture("badbias");
  CHECK(!ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("stackBias"));
  CHECK(any_line_has("REFUSING TO BOOT"));
  CHECK(any_line_has("1 of 10"));
}

// The mirror: a config that claims heap globals the guest does not store. That is the OTHER direction of
// the ABSENT question, and getting it wrong injects writes the real game never makes.
static void test_claimed_but_absent_heap_globals_are_refused(void) {
  FakeRam ram; emit_crt0(ram, x4_guest());
  GameConfig cfg = x4_cfg();
  cfg.heapSizePtr = 0x80123456u; cfg.heapBasePtr = 0x8012345Au;   // invented, to keep the framework quiet
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-fakeptrs");
  capture_stop();
  dump_capture("fakeptrs");
  CHECK(!ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("heapSizePtr"));
  CHECK(any_line_has("heapBasePtr"));
  CHECK(any_line_has("2 of 10"));
}

// …and the reverse omission: the guest DOES store them, the config says 0. Left unfixed this is a port
// silently skipping two writes the real game makes.
static void test_omitted_but_present_heap_globals_are_refused(void) {
  FakeRam ram; emit_crt0(ram, t2_guest());
  GameConfig cfg = t2_cfg();
  cfg.heapSizePtr = 0; cfg.heapBasePtr = 0;
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-omitted");
  capture_stop();
  dump_capture("omitted");
  CHECK(!ok);
  CHECK(any_line_has("heapSizePtr: guest says 0x800ABEF8, game_config.cpp ships 0x00000000"));
}

// Every other field, one at a time — so the audit is proved to actually COMPARE each row rather than
// only the two the fix touched.
static void test_every_field_is_actually_compared(void) {
  struct Case { const char* name; uint32_t GameConfig::*field; };
  const Case cases[] = {
    {"bssZeroLo", &GameConfig::bssZeroLo}, {"bssZeroHi", &GameConfig::bssZeroHi},
    {"stackTopBase", &GameConfig::stackTopBase}, {"stackTopBase2", &GameConfig::stackTopBase2},
    {"heapBase", &GameConfig::heapBase}, {"gp", &GameConfig::gp}, {"libcInit", &GameConfig::libcInit},
  };
  FakeRam ram; emit_crt0(ram, t2_guest());
  for (const Case& k : cases) {
    GameConfig cfg = t2_cfg();
    (cfg.*(k.field)) += 4u;                       // a 4-byte lie: the smallest one that is still a lie
    const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
    capture_start();
    const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-field");
    capture_stop();
    if (ok || !any_line_has(k.name)) dump_capture(k.name);
    fprintf(stderr, "  [field] %-14s perturbed -> audit %s\n", k.name, ok ? "PASSED (BUG)" : "refused");
    CHECK(!ok);
    CHECK(any_line_has(k.name));
  }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 3: THE HONEST NEGATIVE. Unloaded guest memory reads as zero words. The audit must say it examined
// NOTHING — and must NOT refuse, because "I could not look" is not evidence of a fault.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
static void test_unloaded_guest_memory_reports_nothing_examined(void) {
  FakeRam ram;                                    // deliberately empty
  const GameConfig cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-empty");
  capture_stop();
  dump_capture("empty");
  CHECK(ok);                                      // no false refusal
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("read as 0x00000000"));
  CHECK(any_line_has("examined NOTHING"));
  CHECK(any_line_has("resolved ZERO fields"));
  CHECK(any_line_has("PROVED NOTHING"));
  CHECK(g_warns >= 1);
}

// A game that has not filled in `crt0` cannot be audited at all — and must be told so, with the
// denominator, rather than quietly getting a clean bill of health.
static void test_missing_crt0_entry_says_it_verified_nothing(void) {
  GameConfig cfg = t2_cfg(); cfg.crt0 = 0;
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  FakeRam ram; emit_crt0(ram, t2_guest());
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-nocrt0");
  capture_stop();
  dump_capture("nocrt0");
  CHECK(ok);
  CHECK(any_line_has("NOT AUDITED"));
  CHECK(any_line_has("0 of 10 fields"));
  CHECK(any_line_has("UNVERIFIED hand copy"));
}

// A crt0 whose prologue is truncated must report the fields as UNRESOLVED — not as agreeing, and not as
// disagreeing. This is the case where a silent tool would be most convincing and most wrong.
static void test_truncated_prologue_reports_unresolved_not_agreement(void) {
  FakeRam ram; emit_crt0(ram, t2_guest());
  for (uint32_t a = T2_CRT0 + 40u; a < T2_CRT0 + 200u; a += 4u) ram.w[a] = 0x00000000u;  // wipe the tail
  const GameConfig cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(&cfg, p, [&ram](uint32_t a) { return ram.read(a); }, "audit-trunc");
  capture_stop();
  dump_capture("trunc");
  CHECK(ok);                                      // nothing was DISPROVED
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("unresolved"));
  CHECK(any_line_has("limit of the SCANNER"));
  CHECK(any_line_has("must not be read as"));
}

int main(void) {
  RUN(assembler_matches_measured_bytes);           // first: the fixtures must be real instructions
  RUN(agreeing_config_passes_with_a_denominator);
  RUN(agreeing_x4_shape_passes);
  RUN(wrong_bias_is_refused_and_named);
  RUN(claimed_but_absent_heap_globals_are_refused);
  RUN(omitted_but_present_heap_globals_are_refused);
  RUN(every_field_is_actually_compared);
  RUN(unloaded_guest_memory_reports_nothing_examined);
  RUN(missing_crt0_entry_says_it_verified_nothing);
  RUN(truncated_prologue_reports_unresolved_not_agreement);
  return pt_summary();
}
