// libetc VSync is never a shipping source of fields. A measured negative query reads the title's
// libetc field counter; nonnegative calls remain protected typed frame boundaries.
#include "execution_control.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <csignal>
#include <cstdint>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVSyncAddress = 0x800859A8u;
constexpr uint32_t kWindowEnd = 0x80085B20u;
constexpr uint32_t kSyntheticVBlankCounter = 0x80018000u;

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

void assert_wait_modes_request_frame_boundary(Game &game) {
  const OverrideFn handler = game.platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  for (const int32_t mode : {0, 1, 4}) {
    game.core.r[4] = static_cast<uint32_t>(mode);
    handler(&game.core);
    const auto result = game.core.executionControl().consume();
    CHECK(result.has_value());
    if (result) {
      CHECK_EQ(result->reason, psx::cpu::ExecutionExitReason::FrameBoundary);
    }
  }
}

void assert_missing_counter_aborts(Game &game) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    game.core.r[4] = static_cast<uint32_t>(-1);
    game.platform_hle.lookup(kVSyncAddress)(&game.core);
    _exit(0);
  }
  int status = 0;
  CHECK_EQ(waitpid(child, &status, 0), child);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGABRT);
}

} // namespace

static void test_direct_runtime_installs_one_wait_boundary_and_measured_query() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.vsyncQueryCounterAddress = kSyntheticVBlankCounter;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  game->platform_hle.initBuiltins(); // repeatable even when no title override is installed
  CHECK(game->platform_hle.hasNativeFrameLoopContract());
  game->platform_hle.requireNativeFrameLoopContract();
  game->core.mem_w32(kSyntheticVBlankCounter, 73u);
  const OverrideFn handler = game->platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  game->core.r[4] = static_cast<uint32_t>(-1);
  game->core.r[2] = 0xDEADBEEFu;
  handler(&game->core);
  CHECK_EQ(game->core.r[2], 73u);
  CHECK(!game->core.executionControl().pending());
  CHECK_EQ(game->core.mem_r32(kSyntheticVBlankCounter), 73u);
  game->core.mem_w32(kSyntheticVBlankCounter, 74u);
  handler(&game->core);
  CHECK_EQ(game->core.r[2], 74u);
  CHECK(!game->core.executionControl().pending());
  assert_wait_modes_request_frame_boundary(*game);
}

static void test_legacy_adapter_installs_the_same_wait_boundary() {
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
  assert_wait_modes_request_frame_boundary(*game);
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
  assert_wait_modes_request_frame_boundary(*game);
}

static void test_negative_query_without_measured_counter_refuses() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  assert_missing_counter_aborts(*game);
  CHECK(!game->core.executionControl().pending());
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
  RUN(direct_runtime_installs_one_wait_boundary_and_measured_query);
  RUN(legacy_adapter_installs_the_same_wait_boundary);
  RUN(vsync_boundary_cannot_be_replaced);
  RUN(negative_query_without_measured_counter_refuses);
  RUN(missing_direct_vsync_address_has_no_product_contract);
  RUN(vsync_address_outside_the_declared_window_is_refused);
  return pt_summary();
}
