// Pad::serviceFrame hands one PadDelivery to GameRuntime::deliverPad; StockPadBuffers writes it where a
// fixed-buffer driver polls, and a title that delivers nothing leaves guest RAM alone.

#include "game.h"
#include "game_iface.h"
#include "stock_pad_buffers.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kLegacySlot0 = 0x10000u;
constexpr uint32_t kLegacySlot1 = 0x10100u;
constexpr uint32_t kRuntimeSlot0 = 0x20000u;
constexpr uint32_t kRuntimeSlot1 = 0x20100u;
constexpr uint32_t kSlotPointerTable = 0x20200u;
constexpr uint32_t kTableSlot0 = 0x20300u;

class PadRuntime final : public psx::test::StubGameRuntime {
public:
  explicit PadRuntime(const psx::input::StockPadBuffers::Layout &layout) : buffers_(layout) {}

  void deliverPad(Core &core, const psx::input::PadDelivery &delivery) override {
    ++deliveries;
    buffers_.deliver(core, delivery);
  }

  int deliveries = 0;

private:
  psx::input::StockPadBuffers buffers_;
};

class RecordingRuntime final : public psx::test::StubGameRuntime {
public:
  void deliverPad(Core &, const psx::input::PadDelivery &delivery) override {
    last = delivery;
    ++deliveries;
  }

  psx::input::PadDelivery last{};
  int deliveries = 0;
};

void checkDigitalPacket(Core &core, uint32_t address) {
  CHECK_EQ(core.mem_r8(address), 0x00u);
  CHECK_EQ(core.mem_r8(address + 1), 0x41u);
  CHECK_EQ(core.mem_r8(address + 2), 0xF7u);
  CHECK_EQ(core.mem_r8(address + 3), 0xFFu);
}

constexpr uint16_t kStartHeld = static_cast<uint16_t>(0xFFFFu & ~0x0008u);

void test_each_serviced_frame_delivers_one_packet_to_the_title() {
  RecordingRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->pad.setButtons(kStartHeld);

  game->pad.serviceFrame();

  CHECK_EQ(runtime.deliveries, 1);
  CHECK_EQ(runtime.last.packet[0], 0x00u);
  CHECK_EQ(runtime.last.packet[1], 0x41u);
  CHECK_EQ(runtime.last.packet[2], 0xF7u);
  CHECK_EQ(runtime.last.packet[3], 0xFFu);
}

void test_stock_pad_buffers_write_the_fixed_buffers() {
  PadRuntime runtime({.slot0Buffer = kRuntimeSlot0, .slot1Buffer = kRuntimeSlot1});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK_EQ(game->core.cfg, nullptr);
  game->pad.setButtons(kStartHeld);

  game->pad.serviceFrame();

  CHECK_EQ(runtime.deliveries, 1);
  checkDigitalPacket(game->core, kRuntimeSlot0);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot1), 0xFFu);
}

void test_a_slot_pointer_wins_over_the_fixed_buffer() {
  PadRuntime runtime({.slot0Buffer = kRuntimeSlot0, .slotPointerTable = kSlotPointerTable});
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->core.mem_w32(kSlotPointerTable, kTableSlot0);
  game->core.mem_w8(kRuntimeSlot0, 0xA5u);
  game->pad.setButtons(kStartHeld);

  game->pad.serviceFrame();

  checkDigitalPacket(game->core, kTableSlot0);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot0), 0xA5u);
}

void test_the_legacy_adapter_delivers_to_its_config_buffers() {
  GameConfig config{};
  const GameHooks hooks{};
  config.padSlot0Buf = kLegacySlot0;
  config.padSlot1Buf = kLegacySlot1;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->pad.setButtons(kStartHeld);

  game->pad.serviceFrame();

  checkDigitalPacket(game->core, kLegacySlot0);
  CHECK_EQ(game->core.mem_r8(kLegacySlot1), 0xFFu);
}

void test_a_title_that_delivers_nothing_writes_no_guest_address() {
  psx::test::StubGameRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->core.mem_w8(kRuntimeSlot0, 0xA5u);
  game->core.mem_w8(kRuntimeSlot1, 0x5Au);

  game->pad.serviceFrame();

  CHECK_EQ(game->core.mem_r8(kRuntimeSlot0), 0xA5u);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot1), 0x5Au);
}

} // namespace

int main() {
  RUN(each_serviced_frame_delivers_one_packet_to_the_title);
  RUN(stock_pad_buffers_write_the_fixed_buffers);
  RUN(a_slot_pointer_wins_over_the_fixed_buffer);
  RUN(the_legacy_adapter_delivers_to_its_config_buffers);
  RUN(a_title_that_delivers_nothing_writes_no_guest_address);
  return pt_summary();
}
