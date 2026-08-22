// test_crt0_guest_audit.cpp — THE GATE RULE, for the crt0 boot group.
//
// WHAT IT GATES. Every constant in a game's `GuestProgramImage` crt0 group is a value somebody measured out of
// the executable and typed into `game/core/game_config.cpp`. Nothing compared the typed copy to the
// measurement, which is the defect shape found independently in four of five ports here. `crt0_audit`
// (runtime/recomp/crt0_verify.h) closes it by re-deriving the group from the guest's OWN instruction
// stream at boot and refusing a confirmed disagreement. This file gates the auditor in BOTH directions:
// an agreeing config must pass and say how many fields it checked, and a disagreeing one must REFUSE and
// name the field — including the exact disagreement the pre-fix framework shipped (a hardcoded -8 stack
// bias against a guest that has no bias instruction).
//
// WHERE THE GUEST BYTES COME FROM. `tests/crt0_fixture.h` — the stock PSY-Q crt0 ASSEMBLED
// FROM A SPEC, not a blob recorded out of a game: the framework must not carry fragments of a commercial
// executable, and a recorded blob can be wrong in the same way the decoder is. That header is SHARED with
// `tools/crt0_extract --selftest` on purpose — two hand-written assemblers would drift, and the drift
// would show up as one of the two gates passing against a stream the other does not recognise.
//
// THE ASSEMBLER IS NOT ASSUMED CORRECT — IT IS PINNED TO MEASURED BYTES. `crt0_fixture_pins()` carries
// one row per instruction disassembled out of a real crt0 on 2026-08-12 (scratch/crt0_dump.py over
// Tomba!2 MAIN.EXE / SLUS_005.61), and `test_assembler_matches_measured_bytes` asserts every row BEFORE
// any audit case runs. If the encoder drifts, that test fails first and the audit cases below cannot pass
// vacuously against a fixture nobody recognises.
//
// Hermetic: a std::map stands in for guest RAM, lucent's sink is in-process. No Core, no disc, no GPU.
#include "testutil.h"

#include "crt0_fixture.h"
#include "crt0_verify.h"

#include <lucent/log.h>

#include <map>
#include <string>
#include <vector>

// ── the capture sink ────────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> g_lines;
static int g_errors = 0, g_warns = 0;
static void capture_start(void) {
  g_lines.clear();
  g_errors = g_warns = 0;
  lucent::set_sink([](lucent::Level lv, std::string_view line) {
    g_lines.emplace_back(line);
    if (lv == lucent::Level::Error) {
      g_errors++;
    }
    if (lv == lucent::Level::Warn) {
      g_warns++;
    }
  });
}
static void capture_stop(void) {
  lucent::set_sink(nullptr);
}
static void dump_capture(const char *what) {
  fprintf(stderr, "  [%s] captured %zu line(s), %d error / %d warn\n", what, g_lines.size(), g_errors, g_warns);
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

// ── the memory model ────────────────────────────────────────────────────────────────────────────────
struct FakeRam {
  std::map<uint32_t, uint32_t> w;
  uint32_t read(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0u : it->second;
  }
};

// The prologue comes from the shared fixture (tests/crt0_fixture.h): `Crt0Template`, the emitter, and the two
// measured shapes all live there so this test and `crt0_extract --selftest` assemble the SAME bytes.
static void emit_crt0(FakeRam &ram, const Crt0Template &t) {
  crt0_fixture_emit(t, [&ram](uint32_t a, uint32_t w) {
    ram.w[a] = w;
  });
}
// ── the two shapes, as GuestProgramImages ───────────────────────────────────────────────────────────
// The guest templates are the fixture header's two MEASURED shapes (the crt0 groups of Tomba2Engine and
// megamanx4 `game/core/game_config.cpp`), so these cases describe real crt0s rather than invented ones.
//
// The `GuestProgramImage` under audit is DERIVED from the same template rather than hand-typed a second time.
// That is deliberate: a second hand copy of eleven addresses is exactly the defect class this file
// gates, and a typo in it would surface as a spurious refusal that looks like a framework bug. It costs
// the test nothing, because what the audit actually compares is the config against what `crt0_scan`
// DECODES from the emitted bytes — deriving the config does not short-circuit the encoder→decoder round
// trip, and every refusal case below perturbs the config away from the template on purpose.
static GuestProgramImage cfg_from(const Crt0Template &t) {
  GuestProgramImage c{};
  c.bss.begin = t.bssLo;
  c.bss.end = t.bssHi;
  c.stackTopWordAddress = t.stackTopBase;
  c.stackReserveWordAddress = t.stackTopBase2;
  c.heapBase = t.heapBase;
  c.heapSizeStoreAddress = t.heapSizePtr;
  c.heapBaseStoreAddress = t.heapBasePtr; // 0 = ABSENT, a measured answer
  c.globalPointer = t.gp;
  c.libcInitEntry = t.libcInit;
  c.crt0Entry = t.entry;
  c.stackBias = {true, t.bias};
  return c;
}
static const uint32_t T2_CRT0 = CRT0_FIXTURE_TOMBA2_ENTRY;
static Crt0Template t2_guest(void) {
  return crt0_fixture_psyq_tomba2();
}
static GuestProgramImage t2_cfg(void) {
  return cfg_from(t2_guest());
}
static Crt0Template x4_guest(void) {
  return crt0_fixture_psyq_mmx4();
}
static GuestProgramImage x4_cfg(void) {
  return cfg_from(x4_guest());
}

// A plan is needed only so the audit can print the applied values next to the verified ones.
static Crt0Plan plan_for(const GuestProgramImage &cfg, uint32_t stackWord, uint32_t reserveWord) {
  lucent::set_sink([](lucent::Level, std::string_view) {}); // the plan's own log is not under test
  Crt0Plan p = crt0_plan(&cfg, stackWord, reserveWord, "fixture");
  lucent::set_sink(nullptr);
  return p;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 0: the assembler is pinned to bytes disassembled from a real crt0. Runs FIRST: if these fail,
// every fixture below describes an instruction stream no PSX would execute, and the audit cases would be
// passing against nothing.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
// The pin ROWS live in the fixture header beside the encoder that produces them, and so does the FLOOR
// (`CRT0_FIXTURE_PIN_FLOOR`) — a `for` over an empty list passes while proving nothing, so the COUNT is
// asserted before the loop runs. That floor is what makes "0 mismatches" meaningful rather than a report
// of never having looked. It is shared with `crt0_extract --selftest` rather than re-typed: the floor is
// a fact about the row list, and two copies is one to forget when a row is added.
static void test_assembler_matches_measured_bytes(void) {
  const std::vector<Crt0PinRow> pins = crt0_fixture_pins();
  fprintf(stderr, "  [pins] checking %zu encoder row(s) against bytes measured out of real crt0s\n", pins.size());
  CHECK(pins.size() >= (size_t)CRT0_FIXTURE_PIN_FLOOR);
  int bad = 0;
  for (const Crt0PinRow &p : pins) {
    if (p.got != p.want) {
      bad++;
      fprintf(stderr,
              "  [pins] MISMATCH %s: encoder produced 0x%08X, the executable contains 0x%08X\n",
              p.what,
              p.got,
              p.want);
    }
    CHECK_EQ(p.got, p.want);
  }
  fprintf(stderr, "  [pins] %zu row(s) checked, %d mismatch(es)\n", pins.size(), bad);
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// CASE 1: an AGREEING config passes, and says how many fields it checked. The count is asserted because
// "0 DISAGREE" over 0 fields checked is the exact false pass this whole file exists to prevent.
// ════════════════════════════════════════════════════════════════════════════════════════════════════
static void test_agreeing_config_passes_with_a_denominator(void) {
  FakeRam ram;
  emit_crt0(ram, t2_guest());
  const GuestProgramImage cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-t2");
  capture_stop();
  if (!ok) {
    dump_capture("t2");
  }
  CHECK(ok);
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("10 field(s) AGREE, 0 DISAGREE, 0 unresolved"));
  CHECK(any_line_has("libcInit IS the A(39h) InitHeap thunk"));
  CHECK(any_line_has("a1 IS live at the guest's own jal"));
  CHECK(any_line_has("delay slot is `addi a0,a0,4`"));
}

// The other shape: no bias instruction, X4's indirect stack-top load, and NEITHER heap global.
static void test_agreeing_x4_shape_passes(void) {
  FakeRam ram;
  emit_crt0(ram, x4_guest());
  const GuestProgramImage cfg = x4_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-x4");
  capture_stop();
  if (!ok) {
    dump_capture("x4");
  }
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
  FakeRam ram;
  emit_crt0(ram, x4_guest());
  GuestProgramImage cfg = x4_cfg();
  cfg.stackBias = {true, -8}; // the value the old framework applied to all
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-badbias");
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
  FakeRam ram;
  emit_crt0(ram, x4_guest());
  GuestProgramImage cfg = x4_cfg();
  cfg.heapSizeStoreAddress = 0x80123456u;
  cfg.heapBaseStoreAddress = 0x8012345Au; // invented, to keep the framework quiet
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-fakeptrs");
  capture_stop();
  dump_capture("fakeptrs");
  CHECK(!ok);
  CHECK_EQ(g_errors, 1);
  CHECK(any_line_has("heapSizeStoreAddress"));
  CHECK(any_line_has("heapBaseStoreAddress"));
  CHECK(any_line_has("2 of 10"));
}

// …and the reverse omission: the guest DOES store them, the config says 0. Left unfixed this is a port
// silently skipping two writes the real game makes.
static void test_omitted_but_present_heap_globals_are_refused(void) {
  FakeRam ram;
  emit_crt0(ram, t2_guest());
  GuestProgramImage cfg = t2_cfg();
  cfg.heapSizeStoreAddress = 0;
  cfg.heapBaseStoreAddress = 0;
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-omitted");
  capture_stop();
  dump_capture("omitted");
  CHECK(!ok);
  CHECK(any_line_has("heapSizeStoreAddress: guest says 0x800ABEF8, derived runtime ships 0x00000000"));
}

// Every other field, one at a time — so the audit is proved to actually COMPARE each row rather than
// only the two the fix touched.
static void test_every_field_is_actually_compared(void) {
  struct Case {
    const char *name;
    uint32_t *(*field)(GuestProgramImage &);
  };
  const Case cases[] = {
      {"bssZeroLo",
       [](GuestProgramImage &image) {
         return &image.bss.begin;
       }},
      {"bssZeroHi",
       [](GuestProgramImage &image) {
         return &image.bss.end;
       }},
      {"stackTopWordAddress",
       [](GuestProgramImage &image) {
         return &image.stackTopWordAddress;
       }},
      {"stackReserveWordAddress",
       [](GuestProgramImage &image) {
         return &image.stackReserveWordAddress;
       }},
      {"heapBase",
       [](GuestProgramImage &image) {
         return &image.heapBase;
       }},
      {"gp",
       [](GuestProgramImage &image) {
         return &image.globalPointer;
       }},
      {"libcInit",
       [](GuestProgramImage &image) {
         return &image.libcInitEntry;
       }},
  };
  FakeRam ram;
  emit_crt0(ram, t2_guest());
  for (const Case &k : cases) {
    GuestProgramImage cfg = t2_cfg();
    *k.field(cfg) += 4u; // a 4-byte lie: the smallest one that is still a lie
    const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
    capture_start();
    const bool ok = crt0_audit(
        &cfg,
        p,
        [&ram](uint32_t a) {
          return ram.read(a);
        },
        "audit-field");
    capture_stop();
    if (ok || !any_line_has(k.name)) {
      dump_capture(k.name);
    }
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
  FakeRam ram; // deliberately empty
  const GuestProgramImage cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-empty");
  capture_stop();
  dump_capture("empty");
  CHECK(ok); // no false refusal
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
  GuestProgramImage cfg = t2_cfg();
  cfg.crt0Entry = 0;
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  FakeRam ram;
  emit_crt0(ram, t2_guest());
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-nocrt0");
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
  FakeRam ram;
  emit_crt0(ram, t2_guest());
  for (uint32_t a = T2_CRT0 + 40u; a < T2_CRT0 + 200u; a += 4u) {
    ram.w[a] = 0x00000000u; // wipe the tail
  }
  const GuestProgramImage cfg = t2_cfg();
  const Crt0Plan p = plan_for(cfg, 0x00200000u, 0x00008000u);
  capture_start();
  const bool ok = crt0_audit(
      &cfg,
      p,
      [&ram](uint32_t a) {
        return ram.read(a);
      },
      "audit-trunc");
  capture_stop();
  dump_capture("trunc");
  CHECK(ok); // nothing was DISPROVED
  CHECK_EQ(g_errors, 0);
  CHECK(any_line_has("unresolved"));
  CHECK(any_line_has("limit of the SCANNER"));
  CHECK(any_line_has("must not be read as"));
}

int main(void) {
  RUN(assembler_matches_measured_bytes); // first: the fixtures must be real instructions
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
