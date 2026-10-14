// psx::frame::LogicFrame::begin: the logic-frame clock, the producer-census frame and the pad service, in one call.
#include "game.h"
#include "game_runtime.h"
#include "logic_frame.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kSlot0 = 0x20000u;
constexpr uint32_t kSlot1 = 0x20100u;

class PadRuntime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  const GuestPadBufferLayout *guestPadBufferLayout() const override {
    return &layout_;
  }
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }

private:
  GuestPadBufferLayout layout_{.slot0Buffer = kSlot0, .slot1Buffer = kSlot1};
};

void test_begin_publishes_the_frame_and_services_the_pad() {
  PadRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
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
