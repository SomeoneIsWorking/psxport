// Direct GameRuntime consumers have no legacy GameConfig DMA callback table. Their title-owned
// DMACallback override therefore records callbacks in per-Game native state, and Hle::irqPoll must
// resolve that state without inventing a guest-RAM table.
#include "dma_callbacks.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kCallback = 0x80012100u;
constexpr uint32_t kWrongDirectCallback = 0x80012200u;
constexpr uint32_t kLegacyTable = 0x80012000u;
constexpr uint32_t kDestination = 0x80020000u;
constexpr uint32_t kDicr = 0x1F8010F4u;
constexpr uint32_t kDma3Madr = 0x1F8010B0u;
constexpr uint32_t kDma3Bcr = 0x1F8010B4u;
constexpr uint32_t kDma3Chcr = 0x1F8010B8u;

int callbackCalls = 0;
uint32_t callbackA0 = 0;
uint32_t callbackA1 = 0;

void callback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  callbackA1 = core->r[5];
  core->r[4] = 0x11111111u;
  core->r[5] = 0x22222222u;
}

void installCallback(Game &game) {
  const auto image =
      game.core.imageCatalog().activate("test-main", {kCallback & 0x1FFFFFFFu, (kCallback & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "dma-callback", callback}));
}

class DirectRuntime final : public GameRuntime {
public:
  DirectRuntime() {
    image_.residentText = {kCallback & 0x1FFFFFFFu, (kCallback & 0x1FFFFFFFu) + 4u};
    image_.backtraceText = image_.residentText;
  }

  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  const GuestProgramImage *guestProgramImage() const override {
    return &image_;
  }
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::widescreenOnly();
  }
  bool guestVramIsPicture(const Game &) const override {
    return true;
  }

private:
  GuestProgramImage image_{};
};

std::unique_ptr<Game> freshGame() {
  static DirectRuntime runtime;
  psxport_install_game(runtime);
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  auto game = std::make_unique<Game>();
  installCallback(*game);
  return game;
}

std::unique_ptr<Game> freshLegacyGame() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.recMainLo = kCallback & 0x1FFFFFFFu;
  config.recMainHi = (kCallback & 0x1FFFFFFFu) + 4u;
  config.dmaCallbackTable = kLegacyTable;
  psxport_install_game(&config, &hooks);
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  auto game = std::make_unique<Game>();
  installCallback(*game);
  return game;
}

void completeDma3(Game &game) {
  // DMACallback arms channel 3 plus the master bit. The title override owns this write exactly as
  // the linked Sony routine did; the registry owns only callback identity.
  game.core.mem_w32(kDicr, 0x00880000u);
  game.core.mem_w32(kDma3Madr, kDestination);
  game.core.mem_w32(kDma3Bcr, 1u);
  game.core.mem_w32(kDma3Chcr, 0x11000000u);
}

void test_registration_exchanges_one_typed_channel_entry() {
  auto game = freshGame();

  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::Cdrom), 0u);
  CHECK_EQ(game->dmaCallbacks.exchange(DmaChannel::Cdrom, kCallback), 0u);
  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::Cdrom), kCallback);
  CHECK_EQ(game->dmaCallbacks.exchange(DmaChannel::Cdrom, kWrongDirectCallback), kCallback);
  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::Cdrom), kWrongDirectCallback);
  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::MdecOut), 0u);
}

void test_direct_runtime_dispatches_registered_completion_once() {
  auto game = freshGame();
  game->dmaCallbacks.exchange(DmaChannel::Cdrom, kCallback);
  game->core.r[4] = 0xAAAAAAAAu;
  game->core.r[5] = 0xBBBBBBBBu;

  completeDma3(*game);
  CHECK((game->core.pending_work & Core::PW_IRQ) != 0);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, 0xAAAAAAAAu);
  CHECK_EQ(callbackA1, 0xBBBBBBBBu);
  CHECK_EQ(game->core.r[4], 0xAAAAAAAAu);
  CHECK_EQ(game->core.r[5], 0xBBBBBBBBu);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1); // the taken completion is never delivered twice
}

void test_direct_runtime_without_registration_consumes_without_dispatch() {
  auto game = freshGame();
  completeDma3(*game);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK((game->core.pending_work & Core::PW_IRQ) == 0);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
}

void test_legacy_runtime_keeps_its_guest_callback_table_authoritative() {
  auto game = freshLegacyGame();
  game->core.mem_w32(kLegacyTable + 4u * static_cast<uint32_t>(DmaChannel::Cdrom), kCallback);
  game->dmaCallbacks.exchange(DmaChannel::Cdrom, kWrongDirectCallback);

  completeDma3(*game);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(callbackCalls, 1);
}

} // namespace

int main() {
  RUN(registration_exchanges_one_typed_channel_entry);
  RUN(direct_runtime_dispatches_registered_completion_once);
  RUN(direct_runtime_without_registration_consumes_without_dispatch);
  RUN(legacy_runtime_keeps_its_guest_callback_table_authoritative);
  return pt_summary();
}
