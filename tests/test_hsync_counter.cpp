// Root counter 1 is a read-only hardware observation of deterministic emulated time. It remains
// available to guest code, but it neither delivers a field nor licenses libetc VSync: product VSync
// calls are trapped by PlatformHle.
#include "testutil.h"

#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"

namespace {

constexpr uint32_t kRootCounter1 = 0x1F801110u;

void test_root_counter_one_advances_by_the_video_standard() {
  auto *ntsc = new Game();
  CHECK_EQ(ntsc->core.mem_r16(kRootCounter1), 0);
  CHECK(ntsc->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(ntsc->core.mem_r16(kRootCounter1), DISPLAY_LINES_NTSC);

  auto *pal = new Game();
  CHECK(pal->timing.advanceDisplayFields(1, 1, FIELD_RATE_PAL_MILLIHZ));
  pal->gpu.s_disp_pal = 1;
  CHECK_EQ(pal->core.mem_r16(kRootCounter1), DISPLAY_LINES_PAL);
}

void test_root_counter_reports_intra_field_progress() {
  auto *game = new Game();
  game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ);
  CHECK_EQ(game->core.mem_r16(kRootCounter1), DISPLAY_LINES_NTSC);

  const uint64_t ticksPerField = display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ);
  const uint32_t ticksThroughLine248 =
      static_cast<uint32_t>((ticksPerField + DISPLAY_LINES_NTSC - 1) / DISPLAY_LINES_NTSC * 248u);
  game->timing.advanceGuestInstructionTicks(ticksThroughLine248);
  const uint16_t observed = game->core.mem_r16(kRootCounter1);
  CHECK(observed >= DISPLAY_LINES_NTSC + 248u);
  CHECK(observed < DISPLAY_LINES_NTSC * 2u);
}

void test_invalid_hsync_cadence_does_not_invent_a_counter() {
  EmulatedTime clock;
  clock.advanceInstructions(1'000'000u);
  CHECK_EQ(clock.hSyncCount(0, DISPLAY_LINES_NTSC), 0);
  CHECK_EQ(clock.hSyncCount(FIELD_RATE_NTSC_MILLIHZ, 0), 0);
}

} // namespace

int main() {
  RUN(root_counter_one_advances_by_the_video_standard);
  RUN(root_counter_reports_intra_field_progress);
  RUN(invalid_hsync_cadence_does_not_invent_a_counter);
  return pt_summary();
}
