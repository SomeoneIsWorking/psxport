#include "game.h"
#include "synchronous_task_wait.h"
#include "testutil.h"

namespace {

constexpr uint32_t kTask = 0x801FE000u;
constexpr uint32_t kWaitCounter = 0x1F800198u;
int rngCalls = 0;

uint32_t fixed_rng(Core *) {
  ++rngCalls;
  return 0x1234ABCDu;
}

Game *make_game() {
  static GameHooks hooks{};
  hooks.schedRng = fixed_rng;
  Game *game = new Game();
  game->core.hooks = &hooks;
  game->core.mem_w16(kWaitCounter, 0x4567u);
  game->core.mem_w16(kTask + 0x56u, 0xBEEFu);
  rngCalls = 0;
  return game;
}

void test_flag_two_stamps_without_loading_tick() {
  Game *game = make_game();
  const SyncWaitCompletion result = game->pcSched.completeSyncWait(kTask, 2);
  CHECK(result.stamped);
  CHECK_EQ(result.stamp, 0xABCDu);
  CHECK_EQ(result.waitTicks, 0u);
  CHECK_EQ(result.loadingServices, 0u);
  CHECK_EQ(rngCalls, 1);
  CHECK_EQ(game->core.mem_r16(kTask + 0x56u), 0xABCDu);
  CHECK_EQ(game->core.mem_r16(kWaitCounter), 0x4567u);
  delete game;
}

void test_flag_one_has_no_stamp_or_wait_side_effect() {
  Game *game = make_game();
  const SyncWaitCompletion result = game->pcSched.completeSyncWait(kTask, 1);
  CHECK(!result.stamped);
  CHECK_EQ(result.waitTicks, 0u);
  CHECK_EQ(result.loadingServices, 0u);
  CHECK_EQ(rngCalls, 0);
  CHECK_EQ(game->core.mem_r16(kTask + 0x56u), 0xBEEFu);
  CHECK_EQ(game->core.mem_r16(kWaitCounter), 0x4567u);
  delete game;
}

void test_flag_three_uses_the_same_completion_path() {
  Game *game = make_game();
  const SyncWaitCompletion result = game->pcSched.completeSyncWait(kTask, 3);
  CHECK(result.stamped);
  CHECK_EQ(result.stamp, 0xABCDu);
  CHECK_EQ(result.waitTicks, 0u);
  CHECK_EQ(result.loadingServices, 0u);
  CHECK_EQ(rngCalls, 1);
  CHECK_EQ(game->core.mem_r16(kWaitCounter), 0x4567u);
  delete game;
}

} // namespace

int main() {
  RUN(flag_two_stamps_without_loading_tick);
  RUN(flag_one_has_no_stamp_or_wait_side_effect);
  RUN(flag_three_uses_the_same_completion_path);
  return pt_summary();
}
