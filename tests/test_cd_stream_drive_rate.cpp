// test_cd_stream_drive_rate.cpp — a streamed (XA/STR) read is paced by the DRIVE, not by how fast
// the guest asks.
//
// THE DEFECT THIS GATES. Cd::pumpStream delivered a sector every time the guest's StGetNext found
// none ready, with no rate limit whatsoever — so an STR stream ran as fast as the host CPU could
// walk it. Measured on spider1 (docs/issues/0005): over CINEMAS/ATVILOGO.STR the guest's data head
// covered 2040 sectors while the XA audio head, which IS paced (the SPU consumes 735 frames per
// video field off the real-time field clock), covered 512 — the video ran 3.98x too fast, and
// LOGO.STR reproduced it at 3.97x. ATVILOGO is 2112 sectors, which at double speed is ~14.1 s of
// movie; the port played it in ~3.4 s. Audio could never stay in sync with that, because nothing
// was wrong with the audio.
//
// THE RATE IS A HARDWARE FACT, NOT A TUNABLE. A CD-ROM delivers 75 sectors per second per speed
// multiple, and Setmode bit 0x80 selects double speed (the guest programs mode 0xE0 for these
// movies). So the budget is elapsed_time x 75 or x 150 — there is no constant here to pick by
// tuning until a symptom disappears.
//
// Both classes are exercised: a stream that is BEHIND its budget must be allowed to deliver, and a
// stream that has caught up must be told to wait. A pacer that always says "wait" would deadlock the
// movie, and one that always says "go" is the bug being fixed.
#include "testutil.h"

#include <stdint.h>

#include "cd.h"

static void test_speed_from_mode_is_the_hardware_rate(void) {
  CHECK_EQ(cd_stream_sectors_per_sec(0x00), 75);  // single speed
  CHECK_EQ(cd_stream_sectors_per_sec(0x80), 150); // Setmode bit 0x80 = double speed
  CHECK_EQ(cd_stream_sectors_per_sec(0xE0), 150); // what the guest actually programs for STR
  CHECK_EQ(cd_stream_sectors_per_sec(0x20), 75);  // whole-sector framing does not touch the rate
}

// At t=0 nothing has elapsed, so nothing is owed — but the FIRST sector must not be blocked, or the
// stream can never start. Budget is computed from elapsed time; the caller seeds delivery at start.
static void test_nothing_is_due_before_any_time_has_passed(void) {
  CHECK_EQ(cd_stream_sectors_due(0, 150, 0), 0);
  CHECK_EQ(cd_stream_sectors_due(0, 150, 10), 0);
}

// Behind the budget: deliver the shortfall, up to the per-call bound.
//
// The shortfall is `elapsed x rate - already_delivered`, and the ANSWER is that shortfall clamped to
// CD_STREAM_MAX_BURST. Both halves matter, so the cases below sit either side of the clamp rather
// than only exercising one: a stream pumped once per field is never more than ~2.5 sectors behind,
// so the unclamped branch is the one real playback lives in.
static void test_behind_budget_delivers_the_shortfall(void) {
  // One second at double speed owes 150 in total; with 149 delivered exactly 1 is still due.
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 149), 1);
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 148), 2);
  // Single speed over the same second owes half as many, so 74 delivered leaves exactly 1.
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 75, 74), 1);
  // The rate really is proportional to elapsed time: a third of a second at double speed owes 49
  // (integer division), so 48 delivered leaves 1 and 47 leaves 2.
  CHECK_EQ(cd_stream_sectors_due(333333333ull, 150, 48), 1);
  CHECK_EQ(cd_stream_sectors_due(333333333ull, 150, 47), 2);
  // A shortfall larger than the bound is clamped, not delivered whole.
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 0), CD_STREAM_MAX_BURST);
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 100), CD_STREAM_MAX_BURST);
}

// Caught up or AHEAD: nothing is due. This is the case the shipped code never had — it is what stops
// the movie racing. Never negative, so a caller can use the value as a loop count directly.
static void test_caught_up_or_ahead_is_zero_never_negative(void) {
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 150), 0);
  CHECK_EQ(cd_stream_sectors_due(1000000000ull, 150, 400), 0); // ahead: still 0, not -250
  CHECK_EQ(cd_stream_sectors_due(0, 150, 999999), 0);
}

// A stalled process (descheduled, a long native load) can leave a huge elapsed gap. Delivering the
// whole backlog at once would burst thousands of sectors and be exactly the unpaced behaviour this
// exists to stop, so the per-call answer is bounded — and the bound is stated, not silent.
static void test_a_long_gap_is_bounded_per_call(void) {
  const int due = cd_stream_sectors_due(60ull * 1000000000ull, 150, 0); // a whole minute owed
  CHECK(due > 0);
  CHECK(due <= CD_STREAM_MAX_BURST);
}

int main(void) {
  RUN(speed_from_mode_is_the_hardware_rate);
  RUN(nothing_is_due_before_any_time_has_passed);
  RUN(behind_budget_delivers_the_shortfall);
  RUN(caught_up_or_ahead_is_zero_never_negative);
  RUN(a_long_gap_is_bounded_per_call);
  return pt_summary();
}
