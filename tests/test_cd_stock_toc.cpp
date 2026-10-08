// Exercise the stock libcd command/completion service with parsed-disc track metadata.
#include "cd_control.h"
#include "game.h"
#include "testutil.h"

#include <array>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };
constexpr uint32_t kParameter = 0x80112000u;
constexpr uint32_t kResult = 0x80113000u;
constexpr uint32_t kResponseBytes = 8;
using Response = std::array<uint8_t, kResponseBytes>;

int set_disc_env(const char *value) {
#ifdef _WIN32
  return _putenv_s("PSXPORT_DISC", value ? value : "");
#else
  return value ? setenv("PSXPORT_DISC", value, 1) : unsetenv("PSXPORT_DISC");
#endif
}

void install_two_tracks(Game &game) {
  game.disc.track_count = 2;
  game.disc.tracks[0] = DiscTrackInfo{1, 0, 45'000, 150, 0, 0};
  game.disc.tracks[1] = DiscTrackInfo{2, 45'000, 9'000, 0, 0, 0};
}

void check_bytes(Core &core, const Response &expected) {
  for (uint32_t index = 0; index < expected.size(); ++index) {
    CHECK_EQ(core.mem_r8(kResult + index), expected[index]);
  }
}

void issue_and_sync(Core &core, uint8_t command, uint32_t parameter, const Response &expected) {
  for (uint32_t index = 0; index < expected.size(); ++index) {
    core.mem_w8(kResult + index, 0xA5u);
  }
  core.r[A0] = command;
  core.r[A1] = parameter;
  core.r[A2] = kResult;
  core.r[V0] = 0xDEADBEEFu;
  cd_command_stock_sync(&core);
  CHECK_EQ(core.r[V0], 0u);
  check_bytes(core, expected);

  // The real wrapper calls CdSync with the same result buffer. Erase the first publication
  // so this check proves that completion re-publishes the command's response itself.
  for (uint32_t index = 0; index < expected.size(); ++index) {
    core.mem_w8(kResult + index, 0x5Au);
  }
  core.r[A0] = 0;
  core.r[A1] = kResult;
  core.r[V0] = 0xDEADBEEFu;
  cd_sync_stock_sync(&core);
  CHECK_EQ(core.r[V0], 2u);
  check_bytes(core, expected);
}

void refuse_and_sync(Core &core, uint8_t command, uint32_t parameter) {
  for (uint32_t index = 0; index < kResponseBytes; ++index) {
    core.mem_w8(kResult + index, 0xA5u);
  }
  core.r[A0] = command;
  core.r[A1] = parameter;
  core.r[A2] = kResult;
  cd_command_stock_sync(&core);
  CHECK(core.r[V0] != 0u);
  check_bytes(core, Response{});

  for (uint32_t index = 0; index < kResponseBytes; ++index) {
    core.mem_w8(kResult + index, 0x5Au);
  }
  core.r[A1] = kResult;
  cd_sync_stock_sync(&core);
  check_bytes(core, Response{});
}

void test_gettn_gettd_and_leadout_survive_command_and_sync() {
  auto game = std::make_unique<Game>();
  install_two_tracks(*game);
  auto &core = game->core;
  issue_and_sync(core, 0x13, 0, Response{0x02, 0x01, 0x02});

  core.mem_w8(kParameter, 0x01);
  issue_and_sync(core, 0x14, kParameter, Response{0x02, 0x00, 0x02});
  core.mem_w8(kParameter, 0x02);
  issue_and_sync(core, 0x14, kParameter, Response{0x02, 0x10, 0x02});
  core.mem_w8(kParameter, 0x00); // GetTD(0) is lead-out, not track zero.
  issue_and_sync(core, 0x14, kParameter, Response{0x02, 0x12, 0x02});

  // An ordinary command replaces the pending TOC result; its command and sync buffers stay zero.
  issue_and_sync(core, 0x09, 0, Response{});
}

void test_invalid_track_and_disc_metadata_do_not_reuse_prior_result() {
  auto game = std::make_unique<Game>();
  install_two_tracks(*game);
  auto &core = game->core;
  issue_and_sync(core, 0x13, 0, Response{0x02, 0x01, 0x02});

  core.mem_w8(kParameter, 0x03); // absent track
  refuse_and_sync(core, 0x14, kParameter);

  core.mem_w8(kParameter, 0x1A); // malformed BCD is not a track number
  refuse_and_sync(core, 0x14, kParameter);

  game->disc.tracks[1].number = 1; // duplicate metadata must not advertise a valid TOC
  refuse_and_sync(core, 0x13, 0);
}

void test_control_and_fire_keep_their_result_contract() {
  auto game = std::make_unique<Game>();
  install_two_tracks(*game);
  auto &core = game->core;
  core.r[A0] = 0x13;
  core.r[A1] = 0;
  core.r[A2] = kResult;
  cd_control_sync(&core);
  CHECK_EQ(core.r[V0], 1u);
  check_bytes(core, Response{0x02, 0x01, 0x02});

  core.mem_w8(kResult, 0xA5u);
  core.r[A0] = 0x09;
  core.r[A1] = 0;
  core.r[A2] = kResult; // stale a2 is not an output argument of CdControlF.
  cd_control_fire_sync(&core);
  CHECK_EQ(core.r[V0], 1u);
  CHECK_EQ(core.mem_r8(kResult), 0xA5u);
  core.r[A1] = kResult;
  cd_sync_stock_sync(&core);
  check_bytes(core, Response{});

  // CdControlF has no result buffer, but its later CdSync still publishes the TOC.
  core.mem_w8(kResult, 0xA5u);
  core.r[A0] = 0x13;
  core.r[A1] = 0;
  core.r[A2] = kResult;
  cd_control_fire_sync(&core);
  CHECK_EQ(core.r[V0], 1u);
  CHECK_EQ(core.mem_r8(kResult), 0xA5u);
  core.r[A1] = kResult;
  cd_sync_stock_sync(&core);
  check_bytes(core, Response{0x02, 0x01, 0x02});
}

void test_missing_disc_refuses_toc_command() {
  auto game = std::make_unique<Game>();
  game->disc.env_key = "PSXPORT_DISC";
  const char *previous = std::getenv("PSXPORT_DISC");
  const bool had_previous = previous != nullptr;
  const std::string previous_value = previous ? previous : "";
  CHECK_EQ(set_disc_env("/nonexistent/psxport-cd-stock-toc.chd"), 0);
  auto &core = game->core;
  refuse_and_sync(core, 0x13, 0);
  CHECK_EQ(game->disc.track_count, 0u);
  CHECK_EQ(set_disc_env(had_previous ? previous_value.c_str() : nullptr), 0);
}

} // namespace

int main() {
  RUN(gettn_gettd_and_leadout_survive_command_and_sync);
  RUN(invalid_track_and_disc_metadata_do_not_reuse_prior_result);
  RUN(control_and_fire_keep_their_result_contract);
  RUN(missing_disc_refuses_toc_command);
  return pt_summary();
}
