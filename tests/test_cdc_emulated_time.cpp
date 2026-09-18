// test_cdc_emulated_time.cpp — the CDC drive clock is the EMULATED CPU clock: sector deadlines
// advance with executed instructions and with the display fields the native frame loop delivers,
// never with host wall time.
//
// A guest that busy-polls the drive advances that same clock, so a synchronous libcd wait before
// any field boundary still reaches its deadline (no boot deadlock), and it costs the guest the
// console's number of ticks rather than however many the host executes per millisecond. The field
// clock that owes host turns and the per-field SPU pull read the same clock, so drive, fields and
// audio stay in the console's ratio at any host speed (the A/V drift Vagrant Story issue #25 saw
// came from an instruction-cost drive beside a host-paced SPU pull, not from the clock domain).
// These cases pin the contract: instruction work reaches a deadline, host time alone never does.
#include "testutil.h"

#include "cd_drive_timing.h"
#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"

#include <thread>

namespace {

void arm_sector_deadline(Game *game) {
  game->cdc.reading = 1;
  // Exercise a following-sector event so this clock test does not require media. Initial-sector
  // loading and framing are independently covered by test_cdc_continuous_read.
  game->cdc.first_sector_pending = 0;
  game->cdc.drive_event_armed = 1;
  game->cdc.drive_deadline_ticks = cd_drive_sector_period_cpu_ticks(0xA0);
}

void test_instruction_work_reaches_the_sector_deadline() {
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

void test_delivered_fields_reach_the_sector_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);
  // One single-speed sector period is ~13.3 ms, under one NTSC field: the field boundary alone
  // crosses it, which is what lets a display-waiting title receive sectors without spinning.
  CHECK(game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(game->cdc.drive_event_armed, 0);
  CHECK_EQ(game->cdc.following_sector_ready, 1);
}

void test_host_time_alone_never_fires_a_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);
  // Negative arm: two sector periods of real time with no guest progress. The old wall-locked
  // clock fired here; the emulated clock must not, or a fast host would starve a slow guest.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  game->timing.serviceCdcTickSource();
  CHECK_EQ(game->cdc.drive_event_armed, 1);
  CHECK_EQ(game->cdc.following_sector_ready, 0);
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
  RUN(instruction_work_reaches_the_sector_deadline);
  RUN(delivered_fields_reach_the_sector_deadline);
  RUN(host_time_alone_never_fires_a_deadline);
  RUN(instruction_work_is_not_added_on_top_of_the_field_boundary);
  RUN(two_half_field_deliveries_equal_one_full_field);
  RUN(a_late_cpu_resynchronizes_the_next_field_boundary);
  RUN(zero_or_fractionally_invalid_field_input_is_refused);
  return pt_summary();
}
