// test_pace_plan.cpp — frame pacing must not depend on a window, and must run on the GAME's clock.
//
// WHAT THIS GATES. `runtime/psx/pace_plan.h` is the whole pacing decision. Two USER-flagged
// defects lived in the code it replaces (now `frame_pacer.cpp`), and this file pins
// both of them shut:
//
//   1. HEADLESS WAS NEVER PACED. The shipped rule opened with
//          if (!gpu_has_window() || cfg_on("PSXPORT_NOPACE")) return;
//      so a run without a window ran unthrottled — which means every headless timing number in this
//      project described a program the user never runs, and headless is where essentially every
//      measurement is taken. `PSXPORT_NOPACE` was already the independent "run unpaced" switch, so
//      the window term was redundant AND wrong. USER RULE: "windowed and headless should be equal
//      anyway, it shouldn't change anything in the game. headless just means no window and no audio".
//
//   2. THE PACE CLOCK WAS A LITERAL 60.000 Hz:
//          double interval_ms = quota * 1000.0 / 60.0 / parts;
//      while the CONSUMER of the pacing counts display fields at the game's real rate — NTSC is
//      60000/1001 = 59.940 Hz (spider1 sync_native.cpp `kFieldRateMilliHz`, `vblank_advance`). Two
//      clocks at different rates across one wait loop is a beat.
//
// THE NEGATIVE CONTROL IS BUILT IN, and is a COMMAND rather than a paragraph in a report.
// `legacy_pace()` below is a transcription of the rule psxport shipped at 9890eaa8, window term and
// 60.000 literal included. Compile with -DPSXPORT_TEST_LEGACY_PACE_PLAN to run this suite against
// that rule and watch it go red; the default build additionally asserts that the legacy rule FAILS
// the two properties, so this file cannot pass while modelling nothing.
//
//   RED  : g++ -std=c++20 -I runtime/psx -I tests -DPSXPORT_TEST_LEGACY_PACE_PLAN -o
//          scratch/bin/t_pace tests/test_pace_plan.cpp && scratch/bin/t_pace
//   GREEN: the same without the define, or `ctest -R test_pace_plan`.
//
// Hermetic: no clock, no sleep, no SDL, no GPU, no window, no disc. `pace_plan` takes the time as a
// number, which is exactly why it can be tested at all.

#include "pace_plan.h"
#include "testutil.h"

#include <math.h>

// The two field rates the framework decodes from GP1(0x08) bit 3. Stated here as literals ON PURPOSE:
// a test that imported the constants it is checking would pass no matter what they were changed to.
static const unsigned NTSC_MILLIHZ = 59940u; // 60000/1001 Hz
static const unsigned PAL_MILLIHZ = 50000u;  // 50 Hz

// ---- the rule as shipped at 9890eaa8, kept ONLY as this suite's negative control -------------------
// gpu_native.cpp:1542..1575. Note both defects: `hasWindow` gates pacing, and the interval divides by
// a hardcoded 60.0 rather than by the game's field rate (which the rule had no way to receive).
[[maybe_unused]] static PacePlan legacy_pace(const PaceInputs &in, bool hasWindow) {
  PacePlan p;
  p.nextMs = in.nextMs;
  if (!hasWindow || in.unpaced) {
    return p;
  }
  int parts = in.parts < 1 ? 1 : in.parts;
  int quota = in.quota;
  if (quota < 1) {
    p.quotaUnset = true;
    quota = 1;
  }
  p.paced = true;
  p.intervalMs = (double)quota * 1000.0 / 60.0 / (double)parts;
  double next = (in.seeded ? in.nextMs : in.nowMs) + p.intervalMs;
  if (next > in.nowMs) {
    p.sleepMs = next - in.nowMs;
  } else if (in.nowMs - next > p.intervalMs) {
    next = in.nowMs;
    p.resync = true;
  }
  p.nextMs = next;
  return p;
}

// The unit under test, with the window presented as a parameter the SHIPPED rule simply ignores —
// that asymmetry IS the fix, and expressing it this way is what lets one property cover both rules.
static PacePlan pace(const PaceInputs &in, bool hasWindow) {
#ifdef PSXPORT_TEST_LEGACY_PACE_PLAN
  return legacy_pace(in, hasWindow);
#else
  (void)hasWindow;
  return pace_plan(in);
#endif
}

static bool near_ms(double a, double b, double tol) {
  return fabs(a - b) <= tol;
}

// ---- the input table -------------------------------------------------------------------------------
// Every case is a cadence one of the three ports actually paces at, so a green run has a denominator
// that means something: spyro/spider1 pace once per field (quota 1), Tomba!2 once per 30fps logic
// frame (quota 2), and fps60 halves whichever of those with parts=2.
struct Case {
  const char *name;
  int quota;
  int parts;
  unsigned rate;
};
static const Case CASES[] = {
    {"spyro/spider1: 1 field per call", 1, 1, NTSC_MILLIHZ},
    {"Tomba!2: 2 fields per call", 2, 1, NTSC_MILLIHZ},
    {"fps60 halving a 1-field call", 1, 2, NTSC_MILLIHZ},
    {"fps60 halving a 2-field call", 2, 2, NTSC_MILLIHZ},
    {"PAL, 1 field per call", 1, 1, PAL_MILLIHZ},
    {"unset quota (reported, paced 1)", 0, 1, NTSC_MILLIHZ},
};
static const int NCASES = (int)(sizeof CASES / sizeof CASES[0]);

static PaceInputs mk(const Case &c, double nowMs, double nextMs, bool seeded) {
  PaceInputs in;
  in.unpaced = false;
  in.quota = c.quota;
  in.parts = c.parts;
  in.fieldRateMilliHz = c.rate;
  in.nowMs = nowMs;
  in.nextMs = nextMs;
  in.seeded = seeded;
  return in;
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────
// PROPERTY 1 — the pacing decision is identical with and without a window surface.
// ────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_pacing_is_identical_with_and_without_a_window(void) {
  for (int i = 0; i < NCASES; ++i) {
    const PaceInputs in = mk(CASES[i], /*now*/ 1000.0, /*next*/ 1000.0, /*seeded*/ true);
    const PacePlan win = pace(in, /*hasWindow=*/true);
    const PacePlan hdl = pace(in, /*hasWindow=*/false);
    CHECK_EQ(hdl.paced, win.paced);
    CHECK_EQ(hdl.quotaUnset, win.quotaUnset);
    CHECK(near_ms(hdl.intervalMs, win.intervalMs, 0.0));
    CHECK(near_ms(hdl.sleepMs, win.sleepMs, 0.0));
    CHECK(near_ms(hdl.nextMs, win.nextMs, 0.0));
  }
  CHECK_EQ(NCASES, 6); // the denominator: six cadences compared, both legs each
}

// PSXPORT_NOPACE is the ONE switch. It must suppress pacing in BOTH legs — that is what makes it a
// legitimate replacement for the window term rather than a rename of it.
static void test_nopace_is_the_only_switch_that_suppresses_pacing(void) {
  int suppressed = 0;
  for (int i = 0; i < NCASES; ++i) {
    PaceInputs in = mk(CASES[i], 1000.0, 1000.0, true);
    in.unpaced = true;
    const PacePlan win = pace(in, true);
    const PacePlan hdl = pace(in, false);
    CHECK_EQ(win.paced, 0);
    CHECK_EQ(hdl.paced, 0);
    // A non-pacing call must leave the deadline exactly where it was: turning pacing off and on
    // must not inject a catch-up.
    CHECK(near_ms(win.nextMs, in.nextMs, 0.0));
    CHECK(near_ms(hdl.nextMs, in.nextMs, 0.0));
    ++suppressed;
  }
  CHECK_EQ(suppressed, NCASES);
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────
// PROPERTY 2 — the deadline follows the GAME's field rate, so 59.940 Hz does not sleep on 60.000.
// ────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_the_interval_is_one_field_at_the_games_rate(void) {
  for (int i = 0; i < NCASES; ++i) {
    const Case &c = CASES[i];
    const int quota = c.quota < 1 ? 1 : c.quota; // an unset quota paces at 1 field
    const double want = (double)quota * 1000000.0 / (double)c.rate / (double)c.parts;
    const PacePlan p = pace(mk(c, 1000.0, 1000.0, true), /*hasWindow=*/true);
    CHECK(p.paced);
    CHECK(near_ms(p.intervalMs, want, 1e-9));
  }
  CHECK_EQ(NCASES, 6);
}

// The concrete number, spelled out, because "follows the rate" is easy to satisfy vacuously:
// one NTSC field is 1000/(60000/1001) = 16.68335 ms, NOT the 16.66667 ms a 60.000 Hz literal gives.
static void test_an_ntsc_field_is_not_a_sixtieth_of_a_second(void) {
  PaceInputs in = mk(CASES[0], 1000.0, 1000.0, true);
  const PacePlan p = pace(in, true);
  CHECK(p.paced);
  CHECK(near_ms(p.intervalMs, 16.6833500, 1e-6));
  // And it is measurably NOT the 60.000 Hz interval — 16.7 us per field apart.
  CHECK(fabs(p.intervalMs - 1000.0 / 60.0) > 1.0e-5);
}

static void test_pal_paces_slower_than_ntsc(void) {
  const PacePlan pal = pace(mk(CASES[4], 1000.0, 1000.0, true), true);
  const PacePlan ntsc = pace(mk(CASES[0], 1000.0, 1000.0, true), true);
  CHECK(pal.paced);
  CHECK(ntsc.paced);
  CHECK(near_ms(pal.intervalMs, 20.0, 1e-9));
  CHECK(pal.intervalMs > ntsc.intervalMs);
}

// THE DEFECT, AS THE CONSUMER SEES IT. spider1's `vblank_advance` derives its vblank counter from
// REAL elapsed time at 60000/1001 Hz, and the pacing loop is `while (counter < target) { pace();
// advance(); }`. So over N pacing calls the pacer's accumulated wall-clock target must equal the
// time the consumer's clock needs to produce N fields. A 60.000 Hz pacer is short by 60 ms per
// simulated minute — 3.6 whole fields of beat against the counter it is waiting on.
static void test_the_pacer_and_the_field_counter_do_not_drift(void) {
  const int kCalls = 3600; // one minute of NTSC fields
  const double consumer_ms = (double)kCalls * 1000000.0 / (double)NTSC_MILLIHZ;
  double now = 0.0, next = 0.0;
  bool seeded = false;
  int paced = 0;
  for (int n = 0; n < kCalls; ++n) {
    PaceInputs in = mk(CASES[0], now, next, seeded);
    const PacePlan p = pace(in, /*hasWindow=*/true);
    if (!p.paced) {
      break;
    }
    ++paced;
    next = p.nextMs;
    seeded = true;
    now = next; // a host that always hits its deadline
  }
  CHECK_EQ(paced, kCalls); // the denominator: 3600 pacing calls
  // Within a tenth of a field over a simulated minute. The legacy rule misses by 60.06 ms = 3.6 fields.
  CHECK(near_ms(now, consumer_ms, 1.6683));
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────
// The mechanics of the deadline, unchanged by this work but pinned so the rewrite is an equivalence
// proof rather than a rewrite.
// ────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_an_unseeded_deadline_starts_at_now(void) {
  PaceInputs in = mk(CASES[0], /*now*/ 5000.0, /*next*/ -1.0, /*seeded*/ false);
  const PacePlan p = pace(in, true);
  CHECK(p.paced);
  CHECK(near_ms(p.nextMs, 5000.0 + p.intervalMs, 1e-9));
  CHECK(near_ms(p.sleepMs, p.intervalMs, 1e-9));
}

static void test_a_deadline_already_past_does_not_sleep(void) {
  // The host was late by half an interval: the new deadline is behind `now`, so there is nothing to
  // sleep for — but it is less than a whole interval behind, so the debt is CARRIED, not dropped.
  PaceInputs in = mk(CASES[0], /*now*/ 1020.0, /*next*/ 1000.0, /*seeded*/ true);
  const PacePlan p = pace(in, true);
  CHECK(p.paced);
  CHECK(near_ms(p.sleepMs, 0.0, 0.0));
  CHECK(near_ms(p.nextMs, 1000.0 + p.intervalMs, 1e-9));
  CHECK_EQ(p.resync, 0);
}

static void test_a_hitch_resyncs_instead_of_sprinting(void) {
  // The host lost 500 ms. Catching up would run the game fast for 30 frames; the deadline is reset
  // to now instead and the debt is dropped.
  PaceInputs in = mk(CASES[0], /*now*/ 1500.0, /*next*/ 1000.0, /*seeded*/ true);
  const PacePlan p = pace(in, true);
  CHECK(p.paced);
  CHECK_EQ(p.resync, 1);
  CHECK(near_ms(p.nextMs, 1500.0, 0.0));
  CHECK(near_ms(p.sleepMs, 0.0, 0.0));
}

static void test_parts_below_one_is_clamped_not_a_divide_by_zero(void) {
  Case c = CASES[0];
  c.parts = 0;
  const PacePlan p = pace(mk(c, 1000.0, 1000.0, true), true);
  CHECK(p.paced);
  CHECK(near_ms(p.intervalMs, 1000000.0 / (double)NTSC_MILLIHZ, 1e-9));
}

static void test_an_unset_quota_is_reported_not_silently_guessed(void) {
  const PacePlan p = pace(mk(CASES[5], 1000.0, 1000.0, true), true);
  CHECK(p.paced);
  CHECK_EQ(p.quotaUnset, 1);
  CHECK(near_ms(p.intervalMs, 1000000.0 / (double)NTSC_MILLIHZ, 1e-9));
  // …and a port that DID declare its cadence is not flagged.
  const PacePlan ok = pace(mk(CASES[0], 1000.0, 1000.0, true), true);
  CHECK_EQ(ok.quotaUnset, 0);
}

#ifndef PSXPORT_TEST_LEGACY_PACE_PLAN
// A zero field rate is not a rate. The shipped rule refuses rather than substituting a number —
// the entire point of this change is that the pacing rate is never invented locally. (The legacy
// rule has no rate input at all, so this case is meaningless against it.)
static void test_a_zero_field_rate_refuses_to_pace(void) {
  PaceInputs in = mk(CASES[0], 1000.0, 1000.0, true);
  in.fieldRateMilliHz = 0;
  const PacePlan p = pace_plan(in);
  CHECK_EQ(p.paced, 0);
  CHECK_EQ(p.rateUnset, 1);
  CHECK(near_ms(p.nextMs, 1000.0, 0.0));
}

static void test_unpaced_host_execution_retains_the_guest_field_cadence(void) {
  PaceInputs in = mk(CASES[3], 1000.0, 1000.0, true);
  in.unpaced = true;
  const PacePlan p = pace_plan(in);
  CHECK_EQ(p.paced, 0);
  CHECK_EQ(p.effectiveQuota, 2);
  CHECK_EQ(p.effectiveParts, 2);

  in.quota = 0;
  in.parts = 0;
  const PacePlan normalized = pace_plan(in);
  CHECK_EQ(normalized.paced, 0);
  CHECK_EQ(normalized.quotaUnset, 1);
  CHECK_EQ(normalized.effectiveQuota, 1);
  CHECK_EQ(normalized.effectiveParts, 1);
}

// THE SUITE'S OWN NEGATIVE CONTROL, asserted rather than described: the legacy rule must FAIL both
// properties. If this case ever passes, the properties above have stopped discriminating and every
// green run below it is worthless.
static void test_the_legacy_rule_fails_both_properties(void) {
  const PaceInputs in = mk(CASES[0], 1000.0, 1000.0, true);
  const PacePlan lwin = legacy_pace(in, true);
  const PacePlan lhdl = legacy_pace(in, false);
  CHECK_EQ(lwin.paced, 1);
  CHECK_EQ(lhdl.paced, 0); // property 1 violated: headless is not paced
  CHECK(lwin.paced != lhdl.paced);
  // property 2 violated: the legacy interval is a sixtieth of a second regardless of the game's rate
  CHECK(near_ms(lwin.intervalMs, 1000.0 / 60.0, 1e-12));
  CHECK(fabs(lwin.intervalMs - 1000000.0 / (double)NTSC_MILLIHZ) > 1.0e-5);
  // and the drift the consumer sees: 3600 legacy calls land 60.06 ms short of 3600 NTSC fields
  const double legacy_minute = 3600.0 * (1000.0 / 60.0);
  const double consumer_minute = 3600.0 * 1000000.0 / (double)NTSC_MILLIHZ;
  CHECK(consumer_minute - legacy_minute > 60.0);
  CHECK(consumer_minute - legacy_minute < 60.2);
}
#endif

int main(void) {
  RUN(pacing_is_identical_with_and_without_a_window);
  RUN(nopace_is_the_only_switch_that_suppresses_pacing);
  RUN(the_interval_is_one_field_at_the_games_rate);
  RUN(an_ntsc_field_is_not_a_sixtieth_of_a_second);
  RUN(pal_paces_slower_than_ntsc);
  RUN(the_pacer_and_the_field_counter_do_not_drift);
  RUN(an_unseeded_deadline_starts_at_now);
  RUN(a_deadline_already_past_does_not_sleep);
  RUN(a_hitch_resyncs_instead_of_sprinting);
  RUN(parts_below_one_is_clamped_not_a_divide_by_zero);
  RUN(an_unset_quota_is_reported_not_silently_guessed);
#ifndef PSXPORT_TEST_LEGACY_PACE_PLAN
  RUN(a_zero_field_rate_refuses_to_pace);
  RUN(unpaced_host_execution_retains_the_guest_field_cadence);
  RUN(the_legacy_rule_fails_both_properties);
#endif
  return pt_summary();
}
