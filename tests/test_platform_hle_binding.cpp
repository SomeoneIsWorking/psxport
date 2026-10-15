// The stock library classes bind their located entries into the Game's PlatformHle; zero entries stay
// guest code.

#include "game.h"
#include "platform_hle.h"
#include "stock_libcd.h"
#include "stock_libgpu.h"
#include "stock_libgte.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

// Measured CTR (SCUS_944.26) libgte projection leaves.
constexpr uint32_t kSetGeomScreenAddr = 0x8007781Cu;
constexpr uint32_t kSetGeomOffsetAddr = 0x8007782Cu;
constexpr uint32_t kCdReadAddr = 0x80066A50u;
constexpr uint32_t kCdReadSyncAddr = 0x80066B30u;
// Toy Story 2 (SLUS_008.93): libcd CdGetSector, and libgpu's timeout pair with the two guest words
// the arm publishes.
constexpr uint32_t kCdGetSectorAddr = 0x80091108u;
constexpr uint32_t kGpuTimeoutArmAddr = 0x80088380u;
constexpr uint32_t kGpuTimeoutCheckAddr = 0x800883B4u;
constexpr uint32_t kGpuTimeoutDeadlineWord = 0x8009EC20u;
constexpr uint32_t kGpuTimeoutFlagWord = 0x8009EC24u;

void test_a_title_that_binds_nothing_installs_nothing() {
  psx::test::StubGameRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  runtime.registerOverrides(*game);
  CHECK(game->platform_hle.lookup(kSetGeomScreenAddr) == nullptr);
  CHECK(game->platform_hle.lookup(kCdReadAddr) == nullptr);
  CHECK(game->cd.stockLibrary() == nullptr);
}

void test_stock_libgte_binds_the_projection_leaves() {
  psx::test::StubGameRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  psx::hle::StockLibgte({.setGeomOffset = kSetGeomOffsetAddr, .setGeomScreen = kSetGeomScreenAddr}).bind(*game);

  OverrideFn setScreen = game->platform_hle.lookup(kSetGeomScreenAddr);
  OverrideFn setOffset = game->platform_hle.lookup(kSetGeomOffsetAddr);
  CHECK(setScreen != nullptr);
  CHECK(setOffset != nullptr);

  Core &core = game->core;
  core.gte().bind();
  core.r[4] = 401u;
  if (setScreen) {
    setScreen(&core);
  }
  CHECK_EQ(core.rsub.projParams.geomH(), 401.0f);
  CHECK_EQ(core.r[4], 401u); // SetGeomScreen does not mutate its argument register.
  CHECK(!core.rsub.projParams.geomValid());

  core.r[4] = 172u;
  core.r[5] = 119u;
  if (setOffset) {
    setOffset(&core);
  }
  CHECK_EQ(core.rsub.projParams.geomOfx(), 172.0f);
  CHECK_EQ(core.rsub.projParams.geomOfy(), 119.0f);
  CHECK_EQ(core.r[4], 172u << 16);
  CHECK_EQ(core.r[5], 119u << 16);
  CHECK(core.rsub.projParams.geomValid());
}

void test_stock_libcd_binds_its_located_entries_and_becomes_the_games_libcd() {
  psx::test::StubGameRuntime runtime;
  const psx::cd::StockLibcd libcd(
      {.read = kCdReadAddr, .readSync = kCdReadSyncAddr, .getSector = kCdGetSectorAddr}, {}, 0);
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  libcd.bind(*game);

  CHECK(game->platform_hle.lookup(kCdReadAddr) != nullptr);
  CHECK(game->platform_hle.lookup(kCdReadSyncAddr) != nullptr);
  CHECK(game->platform_hle.lookup(kCdGetSectorAddr) != nullptr);
  CHECK(game->platform_hle.lookup(kCdReadAddr + 4u) == nullptr);
  CHECK(game->cd.stockLibrary() == &libcd);
}

void test_stock_libgpu_binds_the_timeout_pair() {
  psx::test::StubGameRuntime runtime;
  const psx::hle::StockLibgpu libgpu({.timeoutArm = kGpuTimeoutArmAddr,
                                      .timeoutCheck = kGpuTimeoutCheckAddr,
                                      .timeoutDeadlineWord = kGpuTimeoutDeadlineWord,
                                      .timeoutFlagWord = kGpuTimeoutFlagWord});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  libgpu.bind(*game);

  OverrideFn arm = game->platform_hle.lookup(kGpuTimeoutArmAddr);
  OverrideFn check = game->platform_hle.lookup(kGpuTimeoutCheckAddr);
  CHECK(arm != nullptr);
  CHECK(check != nullptr);
  if (!arm || !check) {
    return;
  }

  Core &core = game->core;
  core.mem_w32(kGpuTimeoutDeadlineWord, 0u);
  core.mem_w32(kGpuTimeoutFlagWord, 9u);
  arm(&core);
  CHECK_EQ(core.mem_r32(kGpuTimeoutDeadlineWord), 0x7FFFFFFFu);
  CHECK_EQ(core.mem_r32(kGpuTimeoutFlagWord), 0u);
  core.r[2] = 0xFFFFFFFFu;
  check(&core);
  CHECK_EQ(core.r[2], 0u);
}

// An arm that wrote through an unset word would scribble on guest address 0 and still look like it worked.
void test_gpu_timeout_arm_leaves_an_undeclared_word_untouched_and_unbound_when_absent() {
  psx::test::StubGameRuntime runtime;
  const psx::hle::StockLibgpu libgpu(
      {.timeoutArm = kGpuTimeoutArmAddr, .timeoutDeadlineWord = kGpuTimeoutDeadlineWord});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  libgpu.bind(*game);

  Core &core = game->core;
  core.mem_w32(kGpuTimeoutFlagWord, 7u);
  core.mem_w32(0x80000000u, 0x5A5A5A5Au);
  OverrideFn arm = game->platform_hle.lookup(kGpuTimeoutArmAddr);
  CHECK(arm != nullptr);
  if (arm) {
    arm(&core);
  }
  CHECK_EQ(core.mem_r32(kGpuTimeoutDeadlineWord), 0x7FFFFFFFu);
  CHECK_EQ(core.mem_r32(kGpuTimeoutFlagWord), 7u);
  CHECK_EQ(core.mem_r32(0x80000000u), 0x5A5A5A5Au);
  CHECK(game->platform_hle.lookup(kGpuTimeoutCheckAddr) == nullptr);
}

} // namespace

int main(void) {
  RUN(a_title_that_binds_nothing_installs_nothing);
  RUN(stock_libgte_binds_the_projection_leaves);
  RUN(stock_libcd_binds_its_located_entries_and_becomes_the_games_libcd);
  RUN(stock_libgpu_binds_the_timeout_pair);
  RUN(gpu_timeout_arm_leaves_an_undeclared_word_untouched_and_unbound_when_absent);
  return psx::test::summary();
}
