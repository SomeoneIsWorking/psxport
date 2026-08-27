// test_platform_hle_direct_runtime.cpp — direct runtimes (core.cfg == nullptr) must be able to
// declare their own hardware-sync primitives without instantiating the legacy GameConfig bag.
//
// The seam under test is GameRuntime::platformHlePlan(), a consumer-owned fact slice per
// docs/plans/game-seam-redesign.md ("platform-library entry tables"), consumed by initBuiltins()
// when core.cfg is null.

#include "cd_control.h"
#include "game.h"
#include "game_runtime.h"
#include "hw_bind.h"
#include "platform_hle.h"
#include "testutil.h"

#include <array>
#include <memory>
#include <type_traits>

namespace {

constexpr uint32_t kCustomBindingAddr = 0x800859A8u;
constexpr uint32_t kCustomBindingEnd = 0x80085B20u;

// Measured CTR (SCUS_944.26) libgte projection leaves. They share one verified SCEI-library
// window: SetGeomScreen [0x8007781C,0x8007782C), SetGeomOffset [0x8007782C,0x80077844).
constexpr uint32_t kSetGeomScreenAddr = 0x8007781Cu;
constexpr uint32_t kSetGeomOffsetAddr = 0x8007782Cu;
constexpr uint32_t kSetGeomLeavesEnd = 0x80077844u;
constexpr uint32_t kCdReadAddr = 0x80066A50u;
constexpr uint32_t kCdReadSyncAddr = 0x80066B30u;
constexpr uint32_t kCdReadLeavesEnd = 0x80066C20u;

static_assert(std::extent_v<decltype(PlatformHlePlan::windowLo)> == kPlatformHleWindowCapacity);
static_assert(std::extent_v<decltype(GameConfig::PlatformHleCfg::windowLo)> == kPlatformHleWindowCapacity);

int g_handlerCalls = 0;
void recorded_handler(Core *) {
  ++g_handlerCalls;
}

class SilentRuntime : public GameRuntime {
public:
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }

  const GuestProgramImage *guestProgramImage() const override {
    return &image_;
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void *createContext(Core &) override {
    return &ctx_;
  }
  void destroyContext(void *c) override {
    CHECK_EQ(c, &ctx_);
  }
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}

  GuestProgramImage image_{};
  int ctx_ = 0;
};

class PlannedRuntime final : public SilentRuntime {
public:
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan_;
  }

  PlatformHlePlan plan_{};
};

GuestProgramImage makeImage() {
  GuestProgramImage image{};
  image.residentText = {0x00010000u, 0x00120000u};
  image.backtraceText = {0x00010000u, 0x00130000u};
  return image;
}

} // namespace

static void test_direct_runtime_without_plan_installs_nothing_and_says_so() {
  SilentRuntime runtime;
  runtime.image_ = makeImage();
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  // The honest default: a direct runtime that declares no sync primitives installs none. Any
  // address then misses — including the one that would have been bound — and initBuiltins()
  // must not dereference the absent legacy config to get there.
  game->platform_hle.initBuiltins();
  CHECK_EQ(g_handlerCalls, 0);
  CHECK(game->platform_hle.lookup(kCustomBindingAddr) == nullptr);
  CHECK(game->platform_hle.lookup(kCustomBindingAddr + 0x100) == nullptr);
}

static void test_direct_runtime_plan_binds_entries_inside_the_declared_window() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.bindingCount = 1;
  runtime.plan_.bindings[0] = {kCustomBindingAddr, recorded_handler};
  runtime.plan_.windowLo[0] = kCustomBindingAddr;
  runtime.plan_.windowHi[0] = kCustomBindingEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  // Inside the declared window AND exactly at the bound address: the handler resolves and runs.
  g_handlerCalls = 0;
  OverrideFn fn = game->platform_hle.lookup(kCustomBindingAddr);
  CHECK(fn != nullptr);
  if (fn) {
    fn(&game->core);
  }
  CHECK_EQ(g_handlerCalls, 1);

  // The window guard still protects the table: the byte past the measured body end is refused,
  // and so is engine text far below the library window.
  CHECK(game->platform_hle.lookup(kCustomBindingEnd) == nullptr);
  CHECK(game->platform_hle.lookup(0x80050000u | 0x80000000u) == nullptr);
}

static void test_direct_runtime_plan_binds_standard_libgte_projection_leaves() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.setGeomOffset = kSetGeomOffsetAddr;
  runtime.plan_.setGeomScreen = kSetGeomScreenAddr;
  runtime.plan_.windowLo[0] = kSetGeomScreenAddr;
  runtime.plan_.windowHi[0] = kSetGeomLeavesEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  OverrideFn setScreen = game->platform_hle.lookup(kSetGeomScreenAddr);
  OverrideFn setOffset = game->platform_hle.lookup(kSetGeomOffsetAddr);
  CHECK(setScreen != nullptr);
  CHECK(setOffset != nullptr);

  Core &core = game->core;
  gte_bind(&core);
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

static void test_direct_runtime_standard_libgte_leaves_still_require_the_declared_window() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.setGeomOffset = kSetGeomOffsetAddr;
  runtime.plan_.setGeomScreen = kSetGeomScreenAddr;
  // Admit only SetGeomOffset. The typed SetGeomScreen field must not bypass the standard guard.
  runtime.plan_.windowLo[0] = kSetGeomOffsetAddr;
  runtime.plan_.windowHi[0] = kSetGeomLeavesEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  CHECK(game->platform_hle.lookup(kSetGeomOffsetAddr) != nullptr);
  CHECK(game->platform_hle.lookup(kSetGeomScreenAddr) == nullptr);
}

static void test_direct_runtime_plan_binds_standard_stock_cd_read_leaves() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.cdReadAddress = kCdReadAddr;
  runtime.plan_.cdReadSyncAddress = kCdReadSyncAddr;
  runtime.plan_.windowLo[0] = kCdReadAddr;
  runtime.plan_.windowHi[0] = kCdReadLeavesEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  CHECK(game->platform_hle.lookup(kCdReadAddr) == cd_read_stock_sync);
  CHECK(game->platform_hle.lookup(kCdReadSyncAddr) == cd_readsync_stock_sync);
  CHECK(cd_native_stock_read_owned(game->core));
}

static void test_every_exact_window_slot_is_consumed_without_admitting_overflow() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  constexpr std::array<uint32_t, kPlatformHleWindowCapacity> addresses = {
      0x80084010u, 0x80085020u, 0x80086030u, 0x80087040u};
  for (int i = 0; i < kPlatformHleWindowCapacity; i++) {
    runtime.plan_.windowLo[i] = addresses[i];
    runtime.plan_.windowHi[i] = addresses[i] + 4u;
    runtime.plan_.bindings[i] = {addresses[i], recorded_handler};
  }
  constexpr uint32_t kOverflowAddress = 0x80088050u;
  runtime.plan_.bindings[kPlatformHleWindowCapacity] = {kOverflowAddress, recorded_handler};
  runtime.plan_.bindingCount = kPlatformHleWindowCapacity + 1;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  for (const uint32_t address : addresses) {
    CHECK(game->platform_hle.lookup(address) == recorded_handler);
    CHECK(game->platform_hle.lookup(address + 4u) == nullptr);
  }
  // A fifth binding cannot overflow the declared window set into an implicit broad acceptance.
  CHECK(game->platform_hle.lookup(kOverflowAddress) == nullptr);
}

int main(void) {
  RUN(direct_runtime_without_plan_installs_nothing_and_says_so);
  RUN(direct_runtime_plan_binds_entries_inside_the_declared_window);
  RUN(direct_runtime_plan_binds_standard_libgte_projection_leaves);
  RUN(direct_runtime_standard_libgte_leaves_still_require_the_declared_window);
  RUN(direct_runtime_plan_binds_standard_stock_cd_read_leaves);
  RUN(every_exact_window_slot_is_consumed_without_admitting_overflow);
  return pt_summary();
}
