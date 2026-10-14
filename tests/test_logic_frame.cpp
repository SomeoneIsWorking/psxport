// psx::frame::LogicFrame::begin: the logic-frame clock, the producer-census frame and the pad service, in one call.
#include "game.h"
#include "logic_frame.h"
#include "stock_pad_buffers.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kSlot0 = 0x20000u;
constexpr uint32_t kSlot1 = 0x20100u;

class PadRuntime final : public psx::test::StubGameRuntime {
public:
  void deliverPad(Core &core, const psx::input::PadDelivery &delivery) override {
    buffers_.deliver(core, delivery);
  }

private:
  psx::input::StockPadBuffers buffers_{{.slot0Buffer = kSlot0, .slot1Buffer = kSlot1}};
};

void test_begin_publishes_the_frame_and_services_the_pad() {
  PadRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->pad.setButtons(static_cast<uint16_t>(0xFFFFu & ~0x0008u));
  game->core.mem_w8(kSlot0 + 1, 0x00u);

  psx::frame::LogicFrame::begin(*game, 7u);

  CHECK_EQ(game->timing.logicFrame, 7u);
  CHECK_EQ(game->core.rsub.otAttr.frame(), 7u);
  CHECK_EQ(game->core.mem_r8(kSlot0 + 1), 0x41u);
  CHECK_EQ(game->core.mem_r8(kSlot0 + 2), 0xF7u);
}

} // namespace

int main() {
  RUN(begin_publishes_the_frame_and_services_the_pad);
  return pt_summary();
}
