// Root counter 1 is a hardware observation of deterministic emulated time whose value the guest may
// reset (libapi ResetRCnt writes zero). It neither delivers a field nor licenses libetc VSync:
// product VSync calls are trapped by PlatformHle.
#include "testutil.h"

#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"

namespace {

constexpr uint32_t kRootCounter1 = 0x1F801110u;

void test_root_counter_one_advances_by_the_video_standard() {
  auto *ntsc = new Game();
  CHECK_EQ(ntsc->core.mem_r16(kRootCounter1), 0);
  CHECK(ntsc->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ));
  CHECK_EQ(ntsc->core.mem_r16(kRootCounter1), psx::frame::DISPLAY_LINES_NTSC);

  auto *pal = new Game();
  CHECK(pal->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_PAL_MILLIHZ));
  pal->gpu.s_disp_pal = 1;
  CHECK_EQ(pal->core.mem_r16(kRootCounter1), psx::frame::DISPLAY_LINES_PAL);
}

void test_root_counter_reports_intra_field_progress() {
  auto *game = new Game();
  game->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ);
  CHECK_EQ(game->core.mem_r16(kRootCounter1), psx::frame::DISPLAY_LINES_NTSC);

  const uint64_t ticksPerField = psx::frame::displayFieldCpuTicks(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ);
  const uint32_t ticksThroughLine248 = static_cast<uint32_t>((ticksPerField + psx::frame::DISPLAY_LINES_NTSC - 1) /
                                                             psx::frame::DISPLAY_LINES_NTSC * 248u);
  game->timing.advanceGuestInstructionTicks(ticksThroughLine248);
  const uint16_t observed = game->core.mem_r16(kRootCounter1);
  CHECK(observed >= psx::frame::DISPLAY_LINES_NTSC + 248u);
  CHECK(observed < psx::frame::DISPLAY_LINES_NTSC * 2u);
}

void test_root_counter_one_counts_from_a_guest_write() {
  auto *game = new Game();
  for (int field = 0; field < 3; ++field) {
    game->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ);
  }
  CHECK_EQ(game->core.mem_r16(kRootCounter1), 3 * psx::frame::DISPLAY_LINES_NTSC);
  game->core.mem_w16(kRootCounter1, 0);
  CHECK_EQ(game->core.mem_r16(kRootCounter1), 0);
  game->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ);
  CHECK_EQ(game->core.mem_r16(kRootCounter1), psx::frame::DISPLAY_LINES_NTSC);
  game->core.mem_w16(kRootCounter1, 0xFFF0);
  game->timing.advanceDisplayFields(1, 1, psx::frame::FIELD_RATE_NTSC_MILLIHZ);
  CHECK_EQ(game->core.mem_r16(kRootCounter1), static_cast<uint16_t>(0xFFF0u + psx::frame::DISPLAY_LINES_NTSC));
}

void test_invalid_hsync_cadence_does_not_invent_a_counter() {
  psx::frame::EmulatedTime clock;
  clock.advanceInstructions(1'000'000u);
  CHECK_EQ(clock.hSyncCount(0, psx::frame::DISPLAY_LINES_NTSC), 0);
  CHECK_EQ(clock.hSyncCount(psx::frame::FIELD_RATE_NTSC_MILLIHZ, 0), 0);
}

} // namespace

int main() {
  RUN(root_counter_one_advances_by_the_video_standard);
  RUN(root_counter_reports_intra_field_progress);
  RUN(root_counter_one_counts_from_a_guest_write);
  RUN(invalid_hsync_cadence_does_not_invent_a_counter);
  return pt_summary();
}
