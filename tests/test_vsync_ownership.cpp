// libetc VSync is never a shipping source of fields. Every title supplies only its measured address;
// the framework binds that address to one all-mode abort which cannot be replaced by a game handler.
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <array>
#include <csignal>
#include <cstdint>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

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

void expect_abort(void (*operation)(void *), void *context) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    operation(context);
    _exit(0);
  }
  int status = 0;
  CHECK_EQ(waitpid(child, &status, 0), child);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGABRT);
}

struct TrapCall {
  OverrideFn handler;
  Core *core;
  int32_t mode;
};

void call_trap(void *context) {
  auto *call = static_cast<TrapCall *>(context);
  call->core->r[4] = static_cast<uint32_t>(call->mode);
  call->handler(call->core);
}

void replace_trap(void *context) {
  auto *game = static_cast<Game *>(context);
  game->platform_hle.register_(kVSyncAddress, harmless_handler);
}

void require_contract(void *context) {
  auto *game = static_cast<Game *>(context);
  game->platform_hle.requireNativeFrameLoopContract();
}

void init_builtins(void *context) {
  auto *game = static_cast<Game *>(context);
  game->platform_hle.initBuiltins();
}

void assert_all_modes_abort(Game &game) {
  const OverrideFn handler = game.platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  for (const int32_t mode : std::array<int32_t, 4>{-1, 0, 1, 4}) {
    TrapCall call{handler, &game.core, mode};
    expect_abort(call_trap, &call);
  }
}

} // namespace

static void test_direct_runtime_installs_one_all_mode_trap() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  game->platform_hle.initBuiltins(); // repeatable even when no generated substrate is installed
  game->platform_hle.requireNativeFrameLoopContract();
  assert_all_modes_abort(*game);
}

static void test_legacy_adapter_installs_the_same_all_mode_trap() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.hle.windowLo[0] = kVSyncAddress;
  config.hle.windowHi[0] = kWindowEnd;
  config.hle.vsyncTrap = kVSyncAddress;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  game->platform_hle.requireNativeFrameLoopContract();
  assert_all_modes_abort(*game);
}

static void test_vsync_trap_cannot_be_replaced() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  expect_abort(replace_trap, game.get());
  assert_all_modes_abort(*game);
}

static void test_missing_direct_vsync_address_refuses_the_product_contract() {
  DirectRuntime runtime;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  expect_abort(require_contract, game.get());
}

static void test_vsync_address_outside_the_declared_window_refuses_init() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress + 4u;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  expect_abort(init_builtins, game.get());
}

int main() {
  RUN(direct_runtime_installs_one_all_mode_trap);
  RUN(legacy_adapter_installs_the_same_all_mode_trap);
  RUN(vsync_trap_cannot_be_replaced);
  RUN(missing_direct_vsync_address_refuses_the_product_contract);
  RUN(vsync_address_outside_the_declared_window_refuses_init);
  return pt_summary();
}
