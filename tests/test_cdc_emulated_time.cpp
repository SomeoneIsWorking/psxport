// test_cdc_emulated_time.cpp — CD deadlines live in emulated CPU time, including time spent waiting
// for a display field.
//
// Static-recompiled instructions already advance Timing and service the shipping CDC. Mega Man X4's
// loader instead yields once per VSync, so only counting the few instructions between yields makes a
// 2x sector take roughly 250 fields. These cases require the same deadline to become due under an
// instruction-heavy loop and a yield-heavy loop. The mixed case forbids adding one whole field after
// instructions have already consumed part of that field: display delivery advances TO the boundary,
// not BY an unrelated delay.
#include "testutil.h"

#include "cd_drive_timing.h"
#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"

namespace {

void arm_sector_deadline(Game *game) {
  game->cdc.reading = 1;
  // Exercise a following-sector event so this clock test does not require media. Initial-sector
  // loading and framing are independently covered by test_cdc_continuous_read.
  game->cdc.first_sector_pending = 0;
  game->cdc.drive_event_armed = 1;
  game->cdc.drive_deadline_ticks = cd_drive_sector_period_cpu_ticks(0xA0);
}

void test_instruction_heavy_loop_reaches_the_shipping_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);
  const uint64_t deadline = game->cdc.drive_deadline_ticks;

  game->timing.advanceGuestInstructionTicks(static_cast<uint32_t>(deadline - 1));
  CHECK_EQ(game->cdc.drive_event_armed, 1);
  CHECK_EQ(game->cdc.following_sector_ready, 0);

  game->timing.advanceGuestInstructionTicks(1);
  CHECK_EQ(game->cdc.drive_event_armed, 0);
  CHECK_EQ(game->cdc.following_sector_ready, 1);
  CHECK_EQ(game->cdc.q[game->cdc.q_head].type, 1);
}

void test_yield_heavy_loop_reaches_the_same_shipping_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);
  const uint64_t deadline = game->cdc.drive_deadline_ticks;

  CHECK_EQ(game->cdc.drive_event_armed, 1);
  CHECK_EQ(game->cdc.following_sector_ready, 0);
  CHECK(game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.guestInstructionTicks, 0);
  CHECK(game->timing.emulatedCpuTicks() >= deadline);
  CHECK_EQ(game->cdc.drive_event_armed, 0);
  CHECK_EQ(game->cdc.following_sector_ready, 1);
  CHECK_EQ(game->cdc.q[game->cdc.q_head].type, 1);
}

void test_instruction_work_is_not_added_on_top_of_the_field_boundary() {
  auto *game = new Game();
  const uint64_t field_ticks = display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ);

  game->timing.advanceGuestInstructionTicks(static_cast<uint32_t>(field_ticks / 2));
  CHECK(game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.emulatedCpuTicks(), field_ticks);
}

void test_two_half_field_deliveries_equal_one_full_field() {
  auto *game = new Game();
  const uint64_t field_ticks = display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ);

  CHECK(game->timing.advanceDisplayFields(1, 2, FIELD_RATE_NTSC_MILLIHZ));
  CHECK(game->timing.advanceDisplayFields(1, 2, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.emulatedCpuTicks(), field_ticks);
}

void test_a_late_cpu_resynchronizes_the_next_field_boundary() {
  auto *game = new Game();
  const uint64_t field_ticks = display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ);

  game->timing.advanceGuestInstructionTicks(static_cast<uint32_t>(field_ticks + 10));
  CHECK(game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.emulatedCpuTicks(), field_ticks + 10);
  CHECK(game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.emulatedCpuTicks(), field_ticks * 2 + 10);
}

void test_zero_or_fractionally_invalid_field_input_is_refused() {
  auto *game = new Game();
  CHECK(!game->timing.advanceDisplayFields(1, 1, 0));
  CHECK(!game->timing.advanceDisplayFields(1, 0, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->timing.emulatedCpuTicks(), 0);
}

} // namespace

int main() {
  RUN(instruction_heavy_loop_reaches_the_shipping_deadline);
  RUN(yield_heavy_loop_reaches_the_same_shipping_deadline);
  RUN(instruction_work_is_not_added_on_top_of_the_field_boundary);
  RUN(two_half_field_deliveries_equal_one_full_field);
  RUN(a_late_cpu_resynchronizes_the_next_field_boundary);
  RUN(zero_or_fractionally_invalid_field_input_is_refused);
  return pt_summary();
}
