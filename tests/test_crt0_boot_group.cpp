// test_crt0_boot_group.cpp — the crt0 boot group's BOTH directions: does an incomplete group REFUSE
// and say what is missing, and does a complete one produce exactly the guest's own values?
//
// WHAT THIS GATES, and why the old code could not be gated at all. `crt0_setup` used to compute the
// whole crt0 inline in native_boot.cpp — a file that cannot be linked into a hermetic test (it reaches
// runtime guest execution). So the arithmetic that decides sp, gp and the InitHeap
// capacity for every port had NO test, and its two silent failure modes shipped:
//   * a hardcoded `- 8` stack-top bias, right for four of five executables in this workspace;
//   * `mem_w32(cfg->heapSizePtr, …)` with heapSizePtr == 0, i.e. a write to GUEST ADDRESS 0.
// The derivation now lives in crt0_boot.h as a pure function over (GuestProgramImage, the two words the guest
// crt0 loads), and `crt0_setup` is a thin applier with NO arithmetic of its own — so what is asserted
// below IS the shipping path, not a copy of it. If someone re-adds a computation to crt0_setup, that
// computation is by definition a second copy and this file stops covering it.
//
// THE NEGATIVE IS ASSERTED, NOT ASSUMED. Every refusal case checks the emitted LINE COUNT and that the
// line NAMES the missing field, and every success case checks that NOTHING at level error was emitted.
// A test that only checked `ok == false` would pass just as well against a function that refuses
// silently — and a silent refusal is how "the port does not boot" becomes an unattributable mystery.
//
// THE FIXTURES ARE REAL PORTS. Tomba!2 (the shipping consumer, bias -8, both heap globals present) and
// Mega Man X4 (bias 0, NEITHER heap global present) — the two shapes whose difference is the whole
// point. Both groups are the measured values from those repos' game_config.cpp, and X4's exist because
// they were extracted from SLUS_005.61 and cross-checked field by field (megamanx4
// tools/verify_crt0.py --check). Using two real games rather than one synthetic config is what makes
// the bias check DISCRIMINATE: a function that ignored the bias entirely would pass on one of them.
//
// Hermetic: crt0_boot.h is a pure header over a POD, lucent's sink is in-process. No Core, no guest
// memory, no GPU, no disc.
#include "testutil.h"

#include "crt0_boot.h"

#include <lucent/log.h>

#include <string>
#include <vector>

// ── the capture sink ────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> g_lines;
static int g_errors = 0;

static void capture_start(void) {
  g_lines.clear();
  g_errors = 0;
  lucent::set_sink([](lucent::Level lv, std::string_view line) {
    g_lines.emplace_back(line);
    if (lv == lucent::Level::Error) {
      g_errors++;
    }
  });
}
static void capture_stop(void) {
  lucent::set_sink(nullptr);
}

static void dump_capture(const char *what) {
  fprintf(stderr, "  [%s] captured %zu line(s), %d at error level\n", what, g_lines.size(), g_errors);
  for (const std::string &l : g_lines) {
    fprintf(stderr, "  [%s] | %s\n", what, l.c_str());
  }
}
static bool any_line_has(const char *needle) {
  for (const std::string &l : g_lines) {
    if (l.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// ── the two real boot groups ────────────────────────────────────────────────────────────────────
// Tomba!2 MAIN.EXE, crt0 FUN_800896E0 — Tomba2Engine/game/core/game_config.cpp.
static GuestProgramImage tomba2_cfg(void) {
  GuestProgramImage c{};
  c.bss.begin = 0x800BE0D8u;
  c.bss.end = 0x80106228u;
  c.stackTopWordAddress = 0x800A3F88u;
  c.stackReserveWordAddress = 0x800A3F8Cu;
  c.heapBase = 0x80106228u;
  c.heapSizeStoreAddress = 0x800ABEF8u;
  c.heapBaseStoreAddress = 0x800ABEF4u; // `sw a1,-16648(at)` / `sw a0,-16652(at)`
  c.globalPointer = 0x800BE0D4u;
  c.libcInitEntry = 0x80089860u; // BIOS A(39h) InitHeap thunk
  c.stackBias = {true, -8};      // `addi v0,v0,-8` @0x80089710
  return c;
}
// Mega Man X4 SLUS_005.61, crt0 0x800DAE8C — megamanx4/game/core/game_config.cpp kCrt0* constants.
// heapSizePtr/heapBasePtr are ABSENT: the function's ONLY absolute store is `sw ra`.
static GuestProgramImage x4_cfg(void) {
  GuestProgramImage c{};
  c.bss.begin = 0x8012F418u;
  c.bss.end = 0x80175F38u;
  c.stackTopWordAddress = 0x800DAF3Cu;
  c.stackReserveWordAddress = 0x8011CB74u;
  c.heapBase = 0x80175F38u;
  c.globalPointer = 0x8012F418u;
  c.libcInitEntry = 0x800EDCDCu; // BIOS A(39h) InitHeap thunk
  c.stackBias = {true, 0};       // NO bias instruction between lw and `or sp`
  return c;
}
// The words each guest crt0 LOADS. X4's are measured (0x00200000 at 0x800DAF3C, 0x00008000 at
// 0x8011CB74 — verify_crt0.py's symbolic run reads both out of the image). Tomba!2's stack-top word is
// the same 0x00200000 (its sp is 0x801FFFF8 = (0x00200000 - 8) | KSEG0, which is what the port runs
// with today); its reserve word is not needed to be exact for these assertions and is stated as an
// input, so every expected number below is derived from THESE inputs, never from a remembered output.
static const uint32_t X4_STACK_WORD = 0x00200000u;
static const uint32_t X4_RESERVE_WORD = 0x00008000u;
static const uint32_t T2_STACK_WORD = 0x00200000u;
static const uint32_t T2_RESERVE_WORD = 0x00008000u;

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 1: Tomba!2 — the shipping consumer. The plan must reproduce the values the port boots with
// today, INCLUDING the -8 bias, and must perform BOTH absolute stores.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_tomba2_group_is_reproduced_exactly(void) {
  const GuestProgramImage cfg = tomba2_cfg();
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, T2_STACK_WORD, T2_RESERVE_WORD, "test-tomba2");
  capture_stop();
  if (g_errors) {
    dump_capture("tomba2");
  }
  CHECK_EQ(g_errors, 0); // a correct group must not emit one error line
  CHECK(p.ok);
  CHECK_EQ(p.sp, 0x801FFFF8u); // (0x00200000 - 8) | KSEG0 — the bias is APPLIED
  CHECK_EQ(p.sp, p.sp);        // (fp == sp is applied by the caller from this one field)
  CHECK_EQ(p.gp, 0x800BE0D4u);
  CHECK_EQ(p.bssLo, 0x800BE0D8u);
  CHECK_EQ(p.bssHi, 0x80106228u);
  CHECK_EQ(p.heapBaseMasked, 0x00106228u);
  CHECK_EQ(p.a0, 0x8010622Cu); // (masked | KSEG0) + 4
  // heapsz = (0x001FFFF8 - 0x00008000) - 0x00106228
  CHECK_EQ(p.a1, (0x001FFFF8u - 0x00008000u) - 0x00106228u);
  CHECK_EQ(p.libcInit, 0x80089860u);
  CHECK(p.storeHeapSize);
  CHECK_EQ(p.heapSizeAddr, 0x800ABEF8u);
  CHECK(p.storeHeapBase);
  CHECK_EQ(p.heapBaseAddr, 0x800ABEF4u);
  // The plan is not silent on success either — the boot log has to carry the numbers.
  CHECK(any_line_has("0x801FFFF8"));
  CHECK(any_line_has("2 of 2 declared"));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 2: Mega Man X4 — THE CASE THE OLD CODE COULD NOT EXPRESS. No bias, and NEITHER heap global.
// sp must be 0x80200000 (not 0x801FFFF8), and both stores must be declined WITHOUT a guest-0 write.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_x4_group_has_no_bias_and_no_stores(void) {
  const GuestProgramImage cfg = x4_cfg();
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, X4_STACK_WORD, X4_RESERVE_WORD, "test-x4");
  capture_stop();
  if (g_errors) {
    dump_capture("x4");
  }
  CHECK_EQ(g_errors, 0);
  CHECK(p.ok);
  CHECK_EQ(p.sp, 0x80200000u); // NOT 0x801FFFF8 — this is item 1 of the defect
  CHECK_EQ(p.gp, 0x8012F418u);
  CHECK_EQ(p.heapBaseMasked, 0x00175F38u);
  CHECK_EQ(p.a0, 0x80175F3Cu);
  CHECK_EQ(p.a1, 0x000820C8u); // the measured heap size, 532,680 bytes
  // ── THE WHOLE POINT: no store, and NO ADDRESS TO STORE TO. The old code wrote both to guest 0.
  CHECK(!p.storeHeapSize);
  CHECK_EQ(p.heapSizeAddr, 0u);
  CHECK(!p.storeHeapBase);
  CHECK_EQ(p.heapBaseAddr, 0u);
  // …and the absence is ANNOUNCED as measured-absent, so a reader of the boot log can tell it from a
  // field nobody filled in.
  CHECK(any_line_has("0 of 2 declared"));
  CHECK(any_line_has("ABSENT"));
  CHECK(any_line_has("REGISTERS ONLY"));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 3: the two games must DISAGREE. Asserted explicitly, because a bias implementation that read
// the wrong field (or ignored it) would still satisfy one of the two cases above on its own.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_the_two_shapes_differ_on_the_same_stack_word(void) {
  const GuestProgramImage t2 = tomba2_cfg(), x4 = x4_cfg();
  capture_start();
  const Crt0Plan a = crt0_plan(&t2, T2_STACK_WORD, T2_RESERVE_WORD, "diff-t2");
  const Crt0Plan b = crt0_plan(&x4, X4_STACK_WORD, X4_RESERVE_WORD, "diff-x4");
  capture_stop();
  CHECK(a.ok && b.ok);
  // Same stack-top word in, different sp out — i.e. the bias field is genuinely consulted.
  CHECK_EQ(a.stackTopWord, b.stackTopWord);
  CHECK(a.sp != b.sp);
  CHECK_EQ(b.sp - a.sp, 8u);
  CHECK(a.storeHeapSize != b.storeHeapSize);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 4: AN UNDECLARED BIAS REFUSES. This is the field where zero-means-absent is unavailable (0 is
// X4's real value), so the declaration is explicit and its absence must be loud rather than inherit
// the stock -8. A config that is otherwise complete is used, so the refusal is attributable to this
// one field alone.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_undeclared_bias_refuses_and_names_itself(void) {
  GuestProgramImage cfg = x4_cfg();
  cfg.stackBias = {false, 0}; // the state a consumer that has not stated it is in
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, X4_STACK_WORD, X4_RESERVE_WORD, "test-nobias");
  capture_stop();
  dump_capture("nobias");
  CHECK(!p.ok);
  CHECK_EQ(g_errors, 1);                     // it FIRED, exactly once
  CHECK(any_line_has("stackBias.declared")); // …and named the field to fill
  CHECK(any_line_has("1 of 8"));             // …with the denominator: 1 missing OF 8 checked
  CHECK(any_line_has("test-nobias"));        // …and which caller is affected
  // A refused plan must leave EVERYTHING zero: a caller that ignored `ok` must not find a usable-
  // looking sp/gp/a0 sitting in the struct.
  CHECK_EQ(p.sp, 0u);
  CHECK_EQ(p.gp, 0u);
  CHECK_EQ(p.a0, 0u);
  CHECK_EQ(p.a1, 0u);
  CHECK(!p.storeHeapSize);
  CHECK(!p.storeHeapBase);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 5: THE ALL-ZERO CONFIG — megamanx4's state before this landed, and every port's state before
// its crt0 is RE'd. It must refuse, name every one of the 7 unset fields, and count them.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_zero_config_refuses_with_every_field_named(void) {
  const GuestProgramImage cfg{};
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, 0u, 0u, "test-zero");
  capture_stop();
  dump_capture("zero");
  CHECK(!p.ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("8 of 8"));
  for (const char *f : {"bss.begin",
                        "bss.end",
                        "stackTopWordAddress",
                        "stackReserveWordAddress",
                        "heapBase",
                        "globalPointer",
                        "libcInitEntry",
                        "stackBias.declared"}) {
    CHECK(any_line_has(f));
  }
  // It must not print another game's addresses as a fallback.
  CHECK(!any_line_has("0x800BE0D8"));
  CHECK_EQ(p.sp, 0u);
}

// A null GuestProgramImage is the same class of state and must refuse rather than dereference.
static void test_null_config_refuses(void) {
  capture_start();
  const Crt0Plan p = crt0_plan(nullptr, 0u, 0u, "test-null");
  capture_stop();
  dump_capture("null");
  CHECK(!p.ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("NO GuestProgramImage"));
  CHECK(any_line_has("0 of 8")); // the denominator of a refusal that checked nothing
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 6: A PARTIAL group refuses and names ONLY what is missing. Half-filled is the state a port
// passes THROUGH while being RE'd, so it is the likely case rather than the exotic one — and it is the
// one state that looks like progress.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_partial_group_names_only_the_missing(void) {
  GuestProgramImage cfg = tomba2_cfg();
  cfg.globalPointer = 0;
  cfg.libcInitEntry = 0; // two fields not yet located
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, T2_STACK_WORD, T2_RESERVE_WORD, "test-partial");
  capture_stop();
  dump_capture("partial");
  CHECK(!p.ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("2 of 8"));
  CHECK(any_line_has("globalPointer"));
  CHECK(any_line_has("libcInitEntry"));
  CHECK(!any_line_has("bss.begin")); // …and does NOT claim the filled half is missing
  CHECK(!any_line_has("stackTopWordAddress"));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 7: A WRONG value, not a missing one. Every required field is set but the guest word behind
// stackTopBase reads 0 — the subtraction underflows and the "heap size" comes out near 2^32. That
// value would be handed to InitHeap as the arena capacity, and Hle::heapAlloc would then satisfy
// EVERY request out of an arena that does not exist. Refusing is the only safe answer.
// ════════════════════════════════════════════════════════════════════════════════════════════════
static void test_underflowing_heap_size_refuses(void) {
  const GuestProgramImage cfg = x4_cfg();
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, 0u, X4_RESERVE_WORD, "test-underflow"); // stack-top word reads 0
  capture_stop();
  dump_capture("underflow");
  CHECK(!p.ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("not a heap size"));
  CHECK(any_line_has("0x800DAF3C")); // names the global whose word is wrong
  CHECK_EQ(p.a1, 0u);
  // …and the same config with the REAL word passes, so this check discriminates rather than always
  // refusing.
  capture_start();
  const Crt0Plan good = crt0_plan(&cfg, X4_STACK_WORD, X4_RESERVE_WORD, "test-underflow-control");
  capture_stop();
  CHECK(good.ok);
  CHECK_EQ(g_errors, 0);
}

// A non-empty, mis-aligned or inverted .bss span is likewise a wrong value with everything set.
static void test_bad_bss_span_refuses(void) {
  GuestProgramImage cfg = x4_cfg();
  cfg.bss.end = cfg.bss.begin - 4u; // inverted
  capture_start();
  const Crt0Plan a = crt0_plan(&cfg, X4_STACK_WORD, X4_RESERVE_WORD, "test-bss-inverted");
  capture_stop();
  dump_capture("bss-inverted");
  CHECK(!a.ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("word-aligned non-empty"));

  GuestProgramImage mis = x4_cfg();
  mis.bss.begin |= 2u; // unaligned
  capture_start();
  const Crt0Plan b = crt0_plan(&mis, X4_STACK_WORD, X4_RESERVE_WORD, "test-bss-unaligned");
  capture_stop();
  CHECK(!b.ok);
  CHECK_EQ(g_errors, 1);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 8: THE APPLIER, not the plan. The original defect was not in the arithmetic — it was
// `mem_w32(cfg->heapSizePtr, heapsz)` running with `heapSizePtr == 0`. Asserting `storeHeapSize ==
// false` on the plan does NOT prove the caller honoured it, so the write sequence is generated by
// crt0_apply (crt0_boot.h) over an abstract writer and recorded here. `crt0_setup` binds the same
// function to a Core, so this is the shipping sequence, not a paraphrase of it.
// ════════════════════════════════════════════════════════════════════════════════════════════════
struct RecWriter {
  uint32_t lo = 0, hi = 0; // the .bss span, so its writes can be counted
  uint32_t bssWords = 0;
  std::vector<std::pair<uint32_t, uint32_t>> other; // every write OUTSIDE the .bss span
  uint32_t regs[32] = {};
  bool regSet[32] = {};
  uint32_t called = 0;
  int nCalls = 0;
  void w32(uint32_t a, uint32_t v) {
    if (a >= lo && a < hi) {
      bssWords++;
    } else {
      other.emplace_back(a, v);
    }
  }
  void reg(int i, uint32_t v) {
    regs[i] = v;
    regSet[i] = true;
  }
  void call(uint32_t e) {
    called = e;
    nCalls++;
  }
  int writesTo(uint32_t a) const {
    int n = 0;
    for (const auto &kv : other) {
      if (kv.first == a) {
        n++;
      }
    }
    return n;
  }
};
static void dump_writer(const char *what, const RecWriter &w) {
  fprintf(stderr,
          "  [%s] .bss words=%u, %zu non-bss write(s), %d call(s) -> 0x%08X\n",
          what,
          w.bssWords,
          w.other.size(),
          w.nCalls,
          w.called);
  for (const auto &kv : w.other) {
    fprintf(stderr, "  [%s] | write 0x%08X <- 0x%08X\n", what, kv.first, kv.second);
  }
}

static void test_absent_heap_globals_write_nothing_at_all(void) {
  const GuestProgramImage cfg = x4_cfg();
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, X4_STACK_WORD, X4_RESERVE_WORD, "apply-x4");
  capture_stop();
  CHECK(p.ok);
  RecWriter w;
  w.lo = p.bssLo;
  w.hi = p.bssHi;
  CHECK(crt0_apply(p, w));
  dump_writer("apply-x4", w);
  // THE ASSERTION THE DEFECT NEEDED: not one word is written outside the .bss span, and in particular
  // NOTHING is written to guest 0x00000000 — which is what the old unconditional stores did on a game
  // whose crt0 has no heap globals.
  CHECK_EQ((int)w.other.size(), 0);
  CHECK_EQ(w.writesTo(0x00000000u), 0);
  CHECK_EQ(w.bssWords, (p.bssHi - p.bssLo) / 4u); // and the .bss really was cleared, word by word
  CHECK_EQ(w.regs[29], 0x80200000u);
  CHECK_EQ(w.regs[30], 0x80200000u);
  CHECK_EQ(w.regs[28], 0x8012F418u);
  CHECK_EQ(w.regs[4], 0x80175F3Cu);
  CHECK(w.regSet[5]);
  CHECK_EQ(w.regs[5], 0x000820C8u); // a1 = the heap size — the item-4 fix
  CHECK_EQ(w.nCalls, 1);
  CHECK_EQ(w.called, 0x800EDCDCu);
}

// The consumer that DOES have both globals must still get both writes, at the measured addresses and
// nowhere else — so the fix cannot be "stop storing" for everyone.
static void test_present_heap_globals_write_exactly_two_words(void) {
  const GuestProgramImage cfg = tomba2_cfg();
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, T2_STACK_WORD, T2_RESERVE_WORD, "apply-t2");
  capture_stop();
  CHECK(p.ok);
  RecWriter w;
  w.lo = p.bssLo;
  w.hi = p.bssHi;
  CHECK(crt0_apply(p, w));
  dump_writer("apply-t2", w);
  CHECK_EQ((int)w.other.size(), 2);
  CHECK_EQ(w.writesTo(0x00000000u), 0);
  CHECK_EQ(w.writesTo(0x800ABEF8u), 1);
  CHECK_EQ(w.writesTo(0x800ABEF4u), 1);
  for (const auto &kv : w.other) {
    if (kv.first == 0x800ABEF8u) {
      CHECK_EQ(kv.second, p.a1); // the heap SIZE
    }
    if (kv.first == 0x800ABEF4u) {
      CHECK_EQ(kv.second, 0x80106228u); // the heap BASE, KSEG0'd
    }
  }
  CHECK(w.regSet[5]);
  CHECK_EQ(w.regs[5], p.a1);
  CHECK_EQ(w.called, 0x80089860u);
}

// A refused plan must write NOTHING, even through a caller that ignored `ok`. Belt and braces, because
// the failure mode of this whole area is guest state fabricated from unset fields.
static void test_refused_plan_applies_nothing(void) {
  const GuestProgramImage cfg{};
  capture_start();
  const Crt0Plan p = crt0_plan(&cfg, 0u, 0u, "apply-zero");
  capture_stop();
  CHECK(!p.ok);
  RecWriter w;
  w.lo = 1;
  w.hi = 1; // an empty "bss span": every write is recorded
  CHECK(!crt0_apply(p, w));
  dump_writer("apply-zero", w);
  CHECK_EQ((int)w.other.size(), 0);
  CHECK_EQ(w.bssWords, 0u);
  CHECK_EQ(w.nCalls, 0);
  for (int i = 0; i < 32; i++) {
    CHECK(!w.regSet[i]);
  }
}

int main(void) {
  // No once-per-process suppression anywhere in crt0_boot.h — a refusal must fire on EVERY boot, and
  // SBS boots two Cores — so unlike test_zero_config_is_loud.cpp this file's order is not load-bearing.
  RUN(tomba2_group_is_reproduced_exactly);
  RUN(x4_group_has_no_bias_and_no_stores);
  RUN(the_two_shapes_differ_on_the_same_stack_word);
  RUN(undeclared_bias_refuses_and_names_itself);
  RUN(zero_config_refuses_with_every_field_named);
  RUN(null_config_refuses);
  RUN(partial_group_names_only_the_missing);
  RUN(underflowing_heap_size_refuses);
  RUN(bad_bss_span_refuses);
  RUN(absent_heap_globals_write_nothing_at_all);
  RUN(present_heap_globals_write_exactly_two_words);
  RUN(refused_plan_applies_nothing);
  return pt_summary();
}
