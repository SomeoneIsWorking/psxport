// Sony libetc VSync(1) samples root counter 1, which is clocked once per HBlank. The framework
// used to return zero both from Timing::vsync(mode=1) and from the shipping 0x1F801110 MMIO read,
// so a guest waiting for scanline 248 could never leave its loop. These cases drive both shipping
// seams from the same deterministic emulated-time owner and reject that old constant-zero answer.
#include "testutil.h"

#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"

namespace {

constexpr uint32_t kRootCounter1 = 0x1F801110u;
constexpr unsigned kVSyncModeRegister = 4;
constexpr unsigned kReturnValueRegister = 2;

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

void test_vsync_one_reports_hsyncs_since_the_last_wait() {
  auto *game = new Game();
  game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ);

  game->core.r[kVSyncModeRegister] = 1;
  game->timing.vsync();
  CHECK_EQ(game->core.r[kReturnValueRegister], DISPLAY_LINES_NTSC);

  game->core.r[kVSyncModeRegister] = 0;
  game->timing.vsync();
  game->core.r[kVSyncModeRegister] = 1;
  game->timing.vsync();
  CHECK_EQ(game->core.r[kReturnValueRegister], 0);

  const uint64_t ticksPerField = display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ);
  const uint32_t ticksThroughLine248 =
      static_cast<uint32_t>((ticksPerField + DISPLAY_LINES_NTSC - 1) / DISPLAY_LINES_NTSC * 248u);
  game->timing.advanceGuestInstructionTicks(ticksThroughLine248);
  game->timing.vsync();
  CHECK(game->core.r[kReturnValueRegister] >= 248u);
  CHECK(game->core.r[kReturnValueRegister] < DISPLAY_LINES_NTSC);
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
  RUN(vsync_one_reports_hsyncs_since_the_last_wait);
  RUN(invalid_hsync_cadence_does_not_invent_a_counter);
  return pt_summary();
}
