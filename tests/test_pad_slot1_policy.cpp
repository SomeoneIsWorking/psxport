// Slot-1 controller presence is per-game policy rather than a hardcoded "always absent" packet.

#include "../runtime/recomp/game.h"
#include "../runtime/recomp/legacy_game_config.h"
#include "../runtime/recomp/pad_input.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kSlot0 = 0x10000u;
constexpr uint32_t kSlot1 = 0x20000u;

struct Fixture {
  std::unique_ptr<Game> game = std::make_unique<Game>();
  GameConfig config{};

  Fixture() {
    config.padSlot0Buf = kSlot0;
    config.padSlot1Buf = kSlot1;
    game->core.cfg = &config;
    game->pad.game = game.get();
  }
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
