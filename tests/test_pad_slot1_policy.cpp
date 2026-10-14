// Slot-1 controller presence is per-game policy rather than a hardcoded "always absent" packet.

#include "game.h"
#include "pad_input.h"
#include "stock_pad_buffers.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kSlot0 = 0x10000u;
constexpr uint32_t kSlot1 = 0x20000u;

class PadRuntime final : public psx::test::StubGameRuntime {
public:
  void deliverPad(Core &core, const psx::input::PadDelivery &delivery) override {
    buffers_.deliver(core, delivery);
  }

private:
  psx::input::StockPadBuffers buffers_{{.slot0Buffer = kSlot0, .slot1Buffer = kSlot1}};
};

struct Fixture {
  PadRuntime runtime;
  std::unique_ptr<Game> game = std::make_unique<Game>(runtime);
};

void test_slot1_is_absent_by_default() {
  Fixture fixture;
  fixture.game->pad.setButtons(static_cast<uint16_t>(0xFFFFu & ~0x0008u));
  fixture.game->pad.serviceFrame();
  CHECK_EQ(fixture.game->core.mem_r8(kSlot0), 0x00u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot0 + 1), 0x41u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1), 0xFFu);
}

void test_connected_slot1_carries_the_guest_packet() {
  Fixture fixture;
  fixture.game->pad.setSlot1Connected(true);
  fixture.game->pad.setButtons(static_cast<uint16_t>(0xFFFFu & ~0x0008u));
  fixture.game->pad.serviceFrame();
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1), 0x00u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1 + 1), 0x41u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1 + 2), 0xF7u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1 + 3), 0xFFu);
}

void test_connected_idle_slot1_is_present() {
  Fixture fixture;
  fixture.game->pad.setSlot1Connected(true);
  fixture.game->pad.setButtons(0xFFFFu);
  fixture.game->pad.serviceFrame();
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1), 0x00u);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1 + 2), 0xFFu);
  CHECK_EQ(fixture.game->core.mem_r8(kSlot1 + 3), 0xFFu);
}

} // namespace

int main() {
  RUN(slot1_is_absent_by_default);
  RUN(connected_slot1_carries_the_guest_packet);
  RUN(connected_idle_slot1_is_present);
  return pt_summary();
}
