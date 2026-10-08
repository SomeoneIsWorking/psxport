// Stock libcd's native command service must publish the guest work area that its real command-send
// routine updated. Exercise the shipping HLE registration and command entry points in both runtime
// shapes, including commands that must leave that area untouched.
#include "cd_control.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "testutil.h"

#include <array>
#include <memory>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };
constexpr uint32_t kCommandAddress = 0x80066A60u;
constexpr uint32_t kParameterAddress = 0x80112000u;
constexpr uint32_t kPositionAddress = 0x80113000u;
constexpr uint32_t kModeAddress = 0x80114000u;
constexpr uint32_t kLegacyWorkAreaAddress = 0x80115000u;
constexpr uint8_t kSentinel = 0xA5u;

class DirectRuntime final : public GameRuntime {
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
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }

  PlatformHlePlan plan{};
};

void set_parameter(Core &core, const std::array<uint8_t, 4> &bytes) {
  for (uint32_t index = 0; index < bytes.size(); ++index) {
    core.mem_w8(kParameterAddress + index, bytes[index]);
  }
}

void command(Core &core, OverrideFn entry, uint8_t code, uint32_t parameter) {
  core.r[A0] = code;
  core.r[A1] = parameter;
  core.r[A2] = 0;
  core.r[V0] = 0xDEADBEEFu;
  entry(&core);
}

void test_direct_runtime_publishes_setloc_and_setmode_through_stock_command() {
  DirectRuntime runtime;
  runtime.plan.cdCommandAddress = kCommandAddress;
  runtime.plan.windowLo[0] = kCommandAddress;
  runtime.plan.windowHi[0] = kCommandAddress + 4u;
  runtime.plan.stockCdWorkArea = {kPositionAddress, kModeAddress};
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();
  auto &core = game->core;
  CHECK(core.cfg == nullptr);
  OverrideFn entry = game->platform_hle.lookup(kCommandAddress);
  CHECK(entry == cd_command_stock_sync);

  std::array<uint8_t, 4> position{0x00u, 0x02u, 0x16u, 0x01u};
  set_parameter(core, position);
  for (uint32_t index = 0; index < position.size() + 1u; ++index) {
    core.mem_w8(kPositionAddress + index, kSentinel);
  }
  core.mem_w8(kModeAddress, kSentinel);
  command(core, entry, 0x02u, kParameterAddress);
  CHECK_EQ(core.r[V0], 0u);
  for (uint32_t index = 0; index < position.size(); ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), position[index]);
  }
  CHECK_EQ(core.mem_r8(kPositionAddress + position.size()), kSentinel);
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);

  core.mem_w8(kParameterAddress, 0xE0u);
  command(core, entry, 0x0Eu, kParameterAddress);
  CHECK_EQ(core.r[V0], 0u);
  CHECK_EQ(core.mem_r8(kModeAddress), 0xE0u);
  for (uint32_t index = 0; index < position.size(); ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), position[index]);
  }

  core.mem_w8(kParameterAddress, 0x33u);
  command(core, entry, 0x09u, kParameterAddress);
  CHECK_EQ(core.mem_r8(kModeAddress), 0xE0u);
}

void test_unconfigured_direct_work_area_and_absent_parameters_do_not_write() {
  DirectRuntime runtime;
  runtime.plan.cdCommandAddress = kCommandAddress;
  runtime.plan.windowLo[0] = kCommandAddress;
  runtime.plan.windowHi[0] = kCommandAddress + 4u;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();
  auto &core = game->core;
  OverrideFn entry = game->platform_hle.lookup(kCommandAddress);
  CHECK(entry == cd_command_stock_sync);

  set_parameter(core, {0x00u, 0x02u, 0x16u, 0x01u});
  for (uint32_t index = 0; index < 4u; ++index) {
    core.mem_w8(kPositionAddress + index, kSentinel);
  }
  core.mem_w8(kModeAddress, kSentinel);
  command(core, entry, 0x02u, kParameterAddress);
  command(core, entry, 0x0Eu, kParameterAddress);
  for (uint32_t index = 0; index < 4u; ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), kSentinel);
  }
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);

  runtime.plan.stockCdWorkArea = {kPositionAddress, kModeAddress};
  command(core, entry, 0x02u, 0u);
  command(core, entry, 0x0Eu, 0u);
  for (uint32_t index = 0; index < 4u; ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), kSentinel);
  }
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);
}

void test_legacy_adapter_retains_contiguous_position_and_mode_layout() {
  GameConfig config{};
  GameHooks hooks{};
  config.cdCommand = kCommandAddress;
  config.cdLastPosBuf = kLegacyWorkAreaAddress;
  config.hle.windowLo[0] = kCommandAddress;
  config.hle.windowHi[0] = kCommandAddress + 4u;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();
  game->cd.overridesInit();
  auto &core = game->core;
  CHECK(core.cfg == &config);
  OverrideFn entry = game->platform_hle.lookup(kCommandAddress);
  CHECK(entry == cd_command_stock_sync);

  std::array<uint8_t, 4> position{0x00u, 0x02u, 0x16u, 0x01u};
  set_parameter(core, position);
  command(core, entry, 0x02u, kParameterAddress);
  for (uint32_t index = 0; index < position.size(); ++index) {
    CHECK_EQ(core.mem_r8(kLegacyWorkAreaAddress + index), position[index]);
  }
  core.mem_w8(kParameterAddress, 0xE0u);
  command(core, entry, 0x0Eu, kParameterAddress);
  CHECK_EQ(core.mem_r8(kLegacyWorkAreaAddress + 4u), 0xE0u);
}

} // namespace

int main() {
  RUN(direct_runtime_publishes_setloc_and_setmode_through_stock_command);
  RUN(unconfigured_direct_work_area_and_absent_parameters_do_not_write);
  RUN(legacy_adapter_retains_contiguous_position_and_mode_layout);
  return pt_summary();
}
