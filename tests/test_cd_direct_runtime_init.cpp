// Cd::overridesInit is part of dc_boot_init's shipping path. Direct GameRuntime consumers have no
// legacy GameConfig by contract, so that path must accept an empty title-CD group while leaving
// typed PlatformHlePlan registration to PlatformHle::initBuiltins. Adapter consumers retain the
// existing legacy CD address table.
#include "cd_control.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kDirectCdRead = 0x80066A50u;
constexpr uint32_t kDirectCdWindowEnd = 0x80066B30u;
constexpr uint32_t kDirectDrawSync = 0x80066C00u;
constexpr uint32_t kDirectDrawSyncWindowEnd = 0x80066C10u;
constexpr uint32_t kLegacyCdSync = 0x800647A0u;
constexpr uint32_t kLegacyCdWindowEnd = 0x80064810u;

class DirectRuntime : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

class PlannedDirectRuntime final : public DirectRuntime {
public:
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }

  PlatformHlePlan plan{};
};

void test_empty_direct_runtime_cd_registration_is_valid() {
  DirectRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(game->core.cfg == nullptr);
  game->cd.overridesInit();
  CHECK(game->platform_hle.lookup(kDirectCdRead) == nullptr);
}

void test_direct_runtime_cd_plan_remains_owned_by_platform_hle() {
  PlannedDirectRuntime runtime;
  runtime.plan.cdReadAddress = kDirectCdRead;
  runtime.plan.windowLo[0] = kDirectCdRead;
  runtime.plan.windowHi[0] = kDirectCdWindowEnd;
  runtime.plan.drawSyncAddress = kDirectDrawSync;
  runtime.plan.windowLo[1] = kDirectDrawSync;
  runtime.plan.windowHi[1] = kDirectDrawSyncWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  // This is the shipping dc_boot_init order: the legacy CD registration seam runs first, then
  // FrameLoopShell asks PlatformHle to install the direct runtime's typed plan.
  game->cd.overridesInit();
  game->platform_hle.initBuiltins();

  CHECK(game->core.cfg == nullptr);
  CHECK(game->platform_hle.lookup(kDirectCdRead) == cd_read_stock_sync);
  CHECK(game->platform_hle.lookup(kDirectDrawSync) != nullptr);
}

void test_legacy_runtime_keeps_existing_cd_registration() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.cdSync = kLegacyCdSync;
  config.hle.windowLo[0] = kLegacyCdSync;
  config.hle.windowHi[0] = kLegacyCdWindowEnd;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();

  CHECK_EQ(game->core.cfg, &config);
  game->cd.overridesInit();
  CHECK(game->platform_hle.lookup(kLegacyCdSync) == cd_sync_stock_sync);
}

} // namespace

int main() {
  RUN(empty_direct_runtime_cd_registration_is_valid);
  RUN(direct_runtime_cd_plan_remains_owned_by_platform_hle);
  RUN(legacy_runtime_keeps_existing_cd_registration);
  return pt_summary();
}
