// test_cdc_emulated_time.cpp — the CDC drive clock is WALL-LOCKED: sector deadlines advance with
// real time at the nominal rate, NOT with executed-instruction costs.
//
// Rationale (issue #25, Vagrant Story FMV): during streaming the guest busy-polls the CD status
// register instead of display-waiting, so an instruction-cost-driven drive clock runs at HOST speed
// while the SPU pull advances per display field — the two A/V halves drifted up to ~6% and the XA
// ring saturated. A pure field-count lock was also rejected: libcd's synchronous boot waits on a
// command deadline BEFORE any field boundary, which deadlocked. Wall time advances everywhere,
// always — exactly what a crystal-driven drive needs. These cases pin that contract: instruction
// burn alone never fires a deadline (it must not be able to), and a real-time wait of one sector
// period does.
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

void test_instruction_burn_alone_never_fires_the_wall_locked_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);
  const uint64_t deadline = game->cdc.drive_deadline_ticks;

  // Far more guest work than one sector period: the deadline must NOT move — the drive is not
  // paced by how fast the host executes guest instructions.
  game->timing.advanceGuestInstructionTicks(static_cast<uint32_t>(deadline * 4));
  CHECK_EQ(game->cdc.drive_event_armed, 1);
  CHECK_EQ(game->cdc.following_sector_ready, 0);
}

void test_real_time_reaches_the_shipping_deadline() {
  auto *game = new Game();
  arm_sector_deadline(game);

  // One single-speed sector period is ~13.3 ms; sleep two and service. No field advance is
  // involved — this is the property that un-deadlocks synchronous CD boot.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  game->timing.serviceCdcTickSource();
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
  RUN(instruction_burn_alone_never_fires_the_wall_locked_deadline);
  RUN(real_time_reaches_the_shipping_deadline);
  RUN(instruction_work_is_not_added_on_top_of_the_field_boundary);
  RUN(two_half_field_deliveries_equal_one_full_field);
  RUN(a_late_cpu_resynchronizes_the_next_field_boundary);
  RUN(zero_or_fractionally_invalid_field_input_is_refused);
  return pt_summary();
}
