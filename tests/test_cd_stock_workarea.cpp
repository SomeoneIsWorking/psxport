// Stock libcd's native command service must publish the guest work area that its real command-send
// routine updated, through the shipping HLE registration and command entry points, including commands that
// must leave that area untouched.
#include "game.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "r3000.h"
#include "stock_libcd.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <array>
#include <memory>

namespace {

constexpr uint32_t kCommandAddress = 0x80066A60u;
constexpr uint32_t kParameterAddress = 0x80112000u;
constexpr uint32_t kPositionAddress = 0x80113000u;
constexpr uint32_t kModeAddress = 0x80114000u;
constexpr uint32_t kLastCommandAddress = 0x80116000u;
constexpr uint8_t kSentinel = 0xA5u;

psx::cd::StockLibcd commandLibcd(const psx::cd::StockCommandWorkArea &workArea) {
  return {{.command = kCommandAddress}, workArea, 0};
}

void set_parameter(Core &core, const std::array<uint8_t, 4> &bytes) {
  for (uint32_t index = 0; index < bytes.size(); ++index) {
    core.mem_w8(kParameterAddress + index, bytes[index]);
  }
}

template <uint8_t Code> void setCommand(Core &core, uint32_t parameter) {
  core.r[psx::cpu::kA0] = Code;
  core.r[psx::cpu::kA1] = parameter;
  core.r[psx::cpu::kA2] = 0;
  core.r[psx::cpu::kV0] = 0xDEADBEEFu;
}

template <uint8_t Code> void command(Core &core, OverrideFn entry, uint32_t parameter) {
  setCommand<Code>(core, parameter);
  entry(&core);
}

void test_runtime_publishes_setloc_and_setmode_through_stock_command() {
  psx::test::StubGameRuntime runtime;
  const psx::cd::StockLibcd libcd = commandLibcd({kPositionAddress, kModeAddress});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  libcd.bind(*game);
  auto &core = game->core;
  OverrideFn entry = game->platform_hle.lookup(kCommandAddress);
  CHECK(entry != nullptr);

  std::array<uint8_t, 4> position{0x00u, 0x02u, 0x16u, 0x01u};
  set_parameter(core, position);
  for (uint32_t index = 0; index < position.size() + 1u; ++index) {
    core.mem_w8(kPositionAddress + index, kSentinel);
  }
  core.mem_w8(kModeAddress, kSentinel);
  command<0x02u>(core, entry, kParameterAddress);
  CHECK_EQ(core.r[psx::cpu::kV0], 0u);
  for (uint32_t index = 0; index < position.size(); ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), position[index]);
  }
  CHECK_EQ(core.mem_r8(kPositionAddress + position.size()), kSentinel);
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);

  core.mem_w8(kParameterAddress, 0xE0u);
  command<0x0Eu>(core, entry, kParameterAddress);
  CHECK_EQ(core.r[psx::cpu::kV0], 0u);
  CHECK_EQ(core.mem_r8(kModeAddress), 0xE0u);
  for (uint32_t index = 0; index < position.size(); ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), position[index]);
  }

  core.mem_w8(kParameterAddress, 0x33u);
  command<0x09u>(core, entry, kParameterAddress);
  CHECK_EQ(core.mem_r8(kModeAddress), 0xE0u);
}

void test_unconfigured_work_area_and_absent_parameters_do_not_write() {
  psx::test::StubGameRuntime runtime;
  const psx::cd::StockLibcd unconfigured = commandLibcd({});
  const psx::cd::StockLibcd configured = commandLibcd({kPositionAddress, kModeAddress});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  unconfigured.bind(*game);
  auto &core = game->core;
  OverrideFn entry = game->platform_hle.lookup(kCommandAddress);
  CHECK(entry != nullptr);

  set_parameter(core, {0x00u, 0x02u, 0x16u, 0x01u});
  for (uint32_t index = 0; index < 4u; ++index) {
    core.mem_w8(kPositionAddress + index, kSentinel);
  }
  core.mem_w8(kModeAddress, kSentinel);
  command<0x02u>(core, entry, kParameterAddress);
  command<0x0Eu>(core, entry, kParameterAddress);
  for (uint32_t index = 0; index < 4u; ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), kSentinel);
  }
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);

  configured.bind(*game);
  command<0x02u>(core, entry, 0u);
  command<0x0Eu>(core, entry, 0u);
  for (uint32_t index = 0; index < 4u; ++index) {
    CHECK_EQ(core.mem_r8(kPositionAddress + index), kSentinel);
  }
  CHECK_EQ(core.mem_r8(kModeAddress), kSentinel);
}

// CdLastCom() reads the byte every command send writes, parameter or not.
void test_every_command_publishes_the_last_command_byte() {
  psx::test::StubGameRuntime runtime;
  const psx::cd::StockLibcd libcd = commandLibcd({kPositionAddress, kModeAddress, kLastCommandAddress});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  libcd.bind(*game);
  auto &core = game->core;
  core.mem_w8(kLastCommandAddress, kSentinel);

  setCommand<0x11u>(core, 0u);
  libcd.controlFire(core);
  CHECK_EQ(core.mem_r8(kLastCommandAddress), 0x11u);
  core.mem_w8(kParameterAddress, 0xC8u);
  setCommand<0x0Eu>(core, kParameterAddress);
  libcd.control(core);
  CHECK_EQ(core.mem_r8(kLastCommandAddress), 0x0Eu);
  CHECK_EQ(core.mem_r8(kModeAddress), 0xC8u);
  command<0x09u>(core, game->platform_hle.lookup(kCommandAddress), 0u);
  CHECK_EQ(core.mem_r8(kLastCommandAddress), 0x09u);
}

} // namespace

int main() {
  RUN(runtime_publishes_setloc_and_setmode_through_stock_command);
  RUN(unconfigured_work_area_and_absent_parameters_do_not_write);
  RUN(every_command_publishes_the_last_command_byte);
  return psx::test::summary();
}
