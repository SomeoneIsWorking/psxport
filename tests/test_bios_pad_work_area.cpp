// B0[5B] is a guest-visible BIOS work area. Linked libpad calls its pad-enable
// and pad-disable functions directly, so both must cross the shipping dispatcher.
#include "game.h"
#include "legacy_game_config.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kB0Table = 0x8000F000u;
constexpr uint32_t kWorkBaseSlot = kB0Table + 0x5Bu * sizeof(uint32_t);
constexpr uint32_t kPadEnableOffset = 0x884u;
constexpr uint32_t kPadDisableOffset = 0x894u;
constexpr uint32_t kReturnAddress = 0x80012340u;
constexpr uint32_t kPadBuffer = 0x80020000u;
constexpr uint32_t kPadEnableFlag = 0x000074B8u;

psx::cpu::ExecutionResult callPadCallback(Game &game, uint32_t address) {
  game.core.r[2] = 0xBACCu;
  game.core.r[31] = kReturnAddress;
  return psx::cpu::dispatchGuest(game.core, address, psx::cpu::ExecutionBudget::fromCycles(100));
}

void test_published_pad_callbacks_return_through_shipping_dispatch() {
  auto game = std::make_unique<Game>();
  CHECK(game->hle.dispatchBios('B', 0x57u));
  CHECK_EQ(game->core.r[2], kB0Table);
  const uint32_t workBase = game->core.mem_r32(kWorkBaseSlot);
  CHECK(workBase != 0u);

  const auto enable = callPadCallback(*game, workBase + kPadEnableOffset);
  CHECK(enable.returned());
  CHECK_EQ(game->core.pc, kReturnAddress);
  CHECK_EQ(game->core.r[2], 0xBACCu);
  CHECK_EQ(game->core.mem_r32(kPadEnableFlag), 1u);

  const auto disable = callPadCallback(*game, workBase + kPadDisableOffset);
  CHECK(disable.returned());
  CHECK_EQ(game->core.pc, kReturnAddress);
  CHECK_EQ(game->core.r[2], 0xBACCu);
  CHECK_EQ(game->core.mem_r32(kPadEnableFlag), 0u);
}

void test_unpublished_and_invalid_work_area_are_not_services() {
  auto game = std::make_unique<Game>();
  constexpr uint32_t kExpectedWorkBase = 0x8000E000u;
  CHECK_EQ(psx::cpu::classifyGuestHostDispatch(game->core, kExpectedWorkBase + kPadEnableOffset),
           psx::cpu::GuestHostDispatchKind::Fault);

  CHECK(game->hle.dispatchBios('B', 0x57u));
  game->core.mem_w32(kWorkBaseSlot, 0u);
  CHECK_EQ(psx::cpu::classifyGuestHostDispatch(game->core, kExpectedWorkBase + kPadEnableOffset),
           psx::cpu::GuestHostDispatchKind::Fault);
}

void test_bios_pad_lifecycle_gates_guest_packets_without_changing_native_only_input() {
  auto game = std::make_unique<Game>();
  GameConfig config{};
  config.padSlot0Buf = kPadBuffer;
  game->core.cfg = &config;
  game->pad.setButtons(0xFFF7u);

  // Existing title-native packet users have no BIOS pad lifecycle to gate them.
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer + 2u), 0xF7u);

  CHECK(game->hle.dispatchBios('B', 0x12u)); // InitPAD2
  CHECK_EQ(game->core.mem_r32(kPadEnableFlag), 1u);
  game->core.mem_w8(kPadBuffer, 0xA5u);
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0xA5u);

  CHECK(game->hle.dispatchBios('B', 0x13u)); // StartPAD2
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0x00u);

  // The flag is an ordinary guest word; a game may clear it without calling libpad's leaf.
  game->core.mem_w32(kPadEnableFlag, 0u);
  game->core.mem_w8(kPadBuffer, 0xA5u);
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0xA5u);
  game->core.mem_w32(kPadEnableFlag, 1u);
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0x00u);

  CHECK(game->hle.dispatchBios('B', 0x57u)); // GetB0Table
  const uint32_t workBase = game->core.mem_r32(kWorkBaseSlot);
  CHECK(callPadCallback(*game, workBase + kPadDisableOffset).returned());
  game->core.mem_w8(kPadBuffer, 0xA5u);
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0xA5u);

  CHECK(callPadCallback(*game, workBase + kPadEnableOffset).returned());
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0x00u);

  CHECK(game->hle.dispatchBios('B', 0x14u)); // StopPAD2
  game->core.mem_w8(kPadBuffer, 0xA5u);
  game->pad.serviceFrame();
  CHECK_EQ(game->core.mem_r8(kPadBuffer), 0xA5u);
}

} // namespace

int main() {
  RUN(published_pad_callbacks_return_through_shipping_dispatch);
  RUN(unpublished_and_invalid_work_area_are_not_services);
  RUN(bios_pad_lifecycle_gates_guest_packets_without_changing_native_only_input);
  return pt_summary();
}
