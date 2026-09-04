// libetc VSync is never a shipping source of fields. Every title supplies only its measured address;
// the framework binds that address to one typed frame-boundary exit which a game cannot replace.
#include "execution_control.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <cstdint>
#include <memory>

namespace {

constexpr uint32_t kVSyncAddress = 0x800859A8u;
constexpr uint32_t kWindowEnd = 0x80085B20u;

class DirectRuntime final : public GameRuntime {
public:
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }
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

  PlatformHlePlan plan{};
};

void harmless_handler(Core *) {}

void assert_all_modes_request_frame_boundary(Game &game) {
  const OverrideFn handler = game.platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  for (const int32_t mode : {-1, 0, 1, 4}) {
    game.core.r[4] = static_cast<uint32_t>(mode);
    handler(&game.core);
    const auto result = game.core.executionControl().consume();
    CHECK(result.has_value());
    if (result) {
      CHECK_EQ(result->reason, psx::cpu::ExecutionExitReason::FrameBoundary);
    }
  }
}

} // namespace

static void test_direct_runtime_installs_one_all_mode_boundary() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  game->platform_hle.initBuiltins(); // repeatable even when no title override is installed
  CHECK(game->platform_hle.hasNativeFrameLoopContract());
  game->platform_hle.requireNativeFrameLoopContract();
  assert_all_modes_request_frame_boundary(*game);
}

static void test_legacy_adapter_installs_the_same_all_mode_boundary() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.hle.windowLo[0] = kVSyncAddress;
  config.hle.windowHi[0] = kWindowEnd;
  config.hle.vsyncTrap = kVSyncAddress;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  CHECK(game->platform_hle.hasNativeFrameLoopContract());
  game->platform_hle.requireNativeFrameLoopContract();
  assert_all_modes_request_frame_boundary(*game);
}

static void test_vsync_boundary_cannot_be_replaced() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  CHECK(!game->platform_hle.register_(kVSyncAddress, harmless_handler));
  assert_all_modes_request_frame_boundary(*game);
}

static void test_missing_direct_vsync_address_has_no_product_contract() {
  DirectRuntime runtime;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  CHECK(!game->platform_hle.hasNativeFrameLoopContract());
}

static void test_vsync_address_outside_the_declared_window_is_refused() {
  DirectRuntime runtime;
  runtime.plan.windowLo[0] = kVSyncAddress + 4u;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(!game->platform_hle.register_(kVSyncAddress, harmless_handler));
  CHECK(game->platform_hle.lookup(kVSyncAddress) == nullptr);
}

int main() {
  RUN(direct_runtime_installs_one_all_mode_boundary);
  RUN(legacy_adapter_installs_the_same_all_mode_boundary);
  RUN(vsync_boundary_cannot_be_replaced);
  RUN(missing_direct_vsync_address_has_no_product_contract);
  RUN(vsync_address_outside_the_declared_window_is_refused);
  return pt_summary();
}
