// Pad input resolves one guest-buffer fact source: legacy config when present, otherwise the direct
// GameRuntime layout. A missing direct-runtime declaration is a safe no-write answer, not a null
// GameConfig dereference.

#include "../runtime/psx/game.h"
#include "../runtime/psx/game_runtime.h"
#include "../runtime/psx/legacy_game_config.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kLegacySlot0 = 0x10000u;
constexpr uint32_t kLegacySlot1 = 0x10100u;
constexpr uint32_t kRuntimeSlot0 = 0x20000u;
constexpr uint32_t kRuntimeSlot1 = 0x20100u;

class PadRuntime final : public GameRuntime {
public:
  explicit PadRuntime(const GuestPadBufferLayout *layout) : layout_(layout) {}

  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  const GuestPadBufferLayout *guestPadBufferLayout() const override {
    return layout_;
  }
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }

private:
  const GuestPadBufferLayout *layout_;
};

std::unique_ptr<Game> makeGame(PadRuntime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

void checkDigitalPacket(Core &core, uint32_t address) {
  CHECK_EQ(core.mem_r8(address), 0x00u);
  CHECK_EQ(core.mem_r8(address + 1), 0x41u);
  CHECK_EQ(core.mem_r8(address + 2), 0xF7u);
  CHECK_EQ(core.mem_r8(address + 3), 0xFFu);
}

void test_legacy_config_is_the_adapter_layout_source() {
  const GuestPadBufferLayout runtimeLayout{
      .slot0Buffer = kRuntimeSlot0,
      .slot1Buffer = kRuntimeSlot1,
  };
  PadRuntime runtime(&runtimeLayout);
  std::unique_ptr<Game> game = makeGame(runtime);
  GameConfig config{};
  config.padSlot0Buf = kLegacySlot0;
  config.padSlot1Buf = kLegacySlot1;
  game->core.cfg = &config;
  game->core.mem_w8(kRuntimeSlot0, 0xA5u);
  game->pad.setButtons(static_cast<uint16_t>(0xFFFFu & ~0x0008u));

  game->pad.serviceFrame();

  checkDigitalPacket(game->core, kLegacySlot0);
  CHECK_EQ(game->core.mem_r8(kLegacySlot1), 0xFFu);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot0), 0xA5u);
}

void test_direct_runtime_supplies_the_layout_without_config() {
  const GuestPadBufferLayout runtimeLayout{
      .slot0Buffer = kRuntimeSlot0,
      .slot1Buffer = kRuntimeSlot1,
  };
  PadRuntime runtime(&runtimeLayout);
  std::unique_ptr<Game> game = makeGame(runtime);
  CHECK_EQ(game->core.cfg, nullptr);
  game->pad.setButtons(static_cast<uint16_t>(0xFFFFu & ~0x0008u));

  game->pad.serviceFrame();

  checkDigitalPacket(game->core, kRuntimeSlot0);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot1), 0xFFu);
}

void test_missing_direct_layout_does_not_invent_a_guest_address() {
  PadRuntime runtime(nullptr);
  std::unique_ptr<Game> game = makeGame(runtime);
  game->core.mem_w8(kRuntimeSlot0, 0xA5u);
  game->core.mem_w8(kRuntimeSlot1, 0x5Au);

  game->pad.serviceFrame();

  CHECK_EQ(game->core.mem_r8(kRuntimeSlot0), 0xA5u);
  CHECK_EQ(game->core.mem_r8(kRuntimeSlot1), 0x5Au);
}

} // namespace

int main() {
  RUN(legacy_config_is_the_adapter_layout_source);
  RUN(direct_runtime_supplies_the_layout_without_config);
  RUN(missing_direct_layout_does_not_invent_a_guest_address);
  return pt_summary();
}
