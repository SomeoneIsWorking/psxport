// test_spin_detector.cpp — the guest must never hang silently.
//
// A guest that burns instruction after instruction while the host is owed turns it never takes
// (pending_work & PW_HOST stays set) and without moving out of one code region IS a spin — the
// movie-wait stall measured on Vagrant Story (issue #25): resident 0x80022484 polling a libcd
// result slot forever while CD sectors flowed and DMA3 delivered. The detector's job is to declare
// that precisely, so the process can fail fast NAMING the region instead of freezing a window with
// a dead close button.
//
// Contract under test (spin_detector_sample, runtime/psx/timing.cpp):
//   * ticks accumulate across calls; a DECISION happens only when the window fills — the caller
//     chooses the window, so tests stay fast;
//   * declaring requires BOTH: the host stayed owed a turn through every window (PW_HOST never
//     cleared), and every decision sat within the same ±32KB region as the anchor;
//   * anything else RESETS the run: host got serviced (healthy frame loop), or execution moved to
//     another region (legitimate long compute in other functions);
//   * the first decision only ANCHORS — one starved sample proves nothing about spinning;
//   * thresholds of 0 disable detection entirely (the agent-off switch).
#include "spin_detector.h"
#include "testutil.h"
#include <cstdint>

static bool
sample(SpinDetectorState &st, uint32_t pc, bool starved, uint32_t ticks = 100, uint64_t window = 100, int max_run = 3) {
  return spin_detector_sample(st, pc, starved, ticks, window, max_run);
}

static void test_host_starved_same_region_spins(void) {
  SpinDetectorState st;
  // The anchor decision starts the candidate run (run=1); it never fires itself.
  CHECK(!sample(st, 0x80022484, true));
  CHECK(!sample(st, 0x80022490, true));
  CHECK_EQ(st.run, 2);
  // Third consecutive starved in-region decision reaches max_run=3 -> declared. The caller then
  // reports + aborts; this function's job ends at returning true.
  CHECK(sample(st, 0x80022500, true));
  CHECK_EQ(st.run, 3);
}

static void test_host_getting_turns_resets_the_run(void) {
  SpinDetectorState st;
  CHECK(!sample(st, 0x80022484, true));
  CHECK(!sample(st, 0x80022484, true));
  // The guest yielded: the host turn was serviced, PW_HOST cleared. Whatever the code is doing,
  // it is not starving the host — the run must reset, not accumulate.
  CHECK(!sample(st, 0x80022484, false));
  CHECK_EQ(st.run, 0);
  // And it takes the full run again from here before firing.
  CHECK(!sample(st, 0x80022484, true));
  CHECK(!sample(st, 0x80022484, true));
  CHECK(sample(st, 0x80022484, true));
}

static void test_leaving_the_region_resets_the_run(void) {
  SpinDetectorState st;
  CHECK(!sample(st, 0x80040000, true));
  CHECK(!sample(st, 0x80040100, true));
  // Execution walked far away (> 32KB from the anchor): that is forward progress of a kind, but
  // a starved walk into a NEW region starts its own fresh candidate run anchored there (run=1,
  // not 0) — a migrating spin must not get a free ride by hopping regions every window.
  CHECK(!sample(st, 0x80180000, true));
  CHECK_EQ(st.run, 1);
  CHECK(!sample(st, 0x80180010, true));
  CHECK(sample(st, 0x80180020, true)); // reaches max_run=3 in the new region: declared
}

static void test_first_decision_never_fires(void) {
  // Denominator rule: the very first decision only anchors (run=1) — one starved sample is not
  // yet a spin.
  SpinDetectorState st;
  CHECK(!sample(st, 0x80012340, true));
  CHECK_EQ(st.run, 1);
}

static void test_disabled_thresholds_never_fire(void) {
  // window=0 / runs<=0 is the documented off-switch: nothing may fire, ever.
  SpinDetectorState st;
  for (int i = 0; i < 50; ++i) {
    CHECK(!sample(st, 0x80022484, true, 100, 0, 3));
    CHECK(!sample(st, 0x80022484, true, 100, 100, 0));
  }
  CHECK_EQ(st.run, 0);
}

static void test_partial_window_accumulates(void) {
  // Ticks smaller than the window must accumulate across calls instead of being dropped —
  // otherwise fine-grained callers (per-basic-block tick counts) would never decide at all.
  SpinDetectorState st;
  st.window_ticks = 60;
  CHECK(!sample(st, 0x80022484, true, /*ticks=*/20));
  CHECK(!sample(st, 0x80022484, true, /*ticks=*/20));
  CHECK(!sample(st, 0x80022490, true, /*ticks=*/30)); // window fills HERE (60+20+30>=100)
  CHECK_EQ(st.run, 1);
}

int main(void) {
  RUN(host_starved_same_region_spins);
  RUN(host_getting_turns_resets_the_run);
  RUN(leaving_the_region_resets_the_run);
  RUN(first_decision_never_fires);
  RUN(disabled_thresholds_never_fire);
  RUN(partial_window_accumulates);
  return pt_summary();
}
