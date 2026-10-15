// A title's DMACallback override records callbacks in per-Game native state, and Hle::irqPoll resolves
// that state without inventing a guest-RAM table; a title that binds StockDmaCallbacks reads its own table.
#include "dma_callbacks.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "irq_edge.h"
#include "native_dispatch.h"
#include "platform_hle.h"
#include "stock_dma_callbacks.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kCallback = 0x80012100u;
constexpr uint32_t kOtherCallback = 0x80012200u;
constexpr uint32_t kGuestTable = 0x80012000u;
constexpr uint32_t kDestination = 0x80020000u;
constexpr uint32_t kDicr = 0x1F8010F4u;
constexpr uint32_t kDma3Madr = 0x1F8010B0u;
constexpr uint32_t kDma3Bcr = 0x1F8010B4u;
constexpr uint32_t kDma3Chcr = 0x1F8010B8u;

int callbackCalls = 0;
uint32_t callbackA0 = 0;
uint32_t callbackA1 = 0;
uint32_t callbackSp = 0;

void callback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  callbackA1 = core->r[5];
  callbackSp = core->r[29];
  core->r[4] = 0x11111111u;
  core->r[5] = 0x22222222u;
}

void installCallback(Game &game) {
  const auto image =
      game.core.imageCatalog().activate("test-main", {kCallback & 0x1FFFFFFFu, (kCallback & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "dma-callback", callback}));
}

class DmaRuntime final : public psx::test::StubGameRuntime {
public:
  explicit DmaRuntime(uint32_t guestDmaTable = 0) : guestDmaTable_(guestDmaTable) {}

  void registerOverrides(Game &game) override {
    if (guestDmaTable_ != 0) {
      guestTable_.bind(game);
    }
  }

private:
  uint32_t guestDmaTable_;
  const psx::hle::StockDmaCallbacks guestTable_{guestDmaTable_};
};

std::unique_ptr<DmaRuntime> sharedRuntime;
std::unique_ptr<DmaRuntime> guestTableRuntime;

std::unique_ptr<Game> freshGame() {
  DmaRuntime &runtime = *sharedRuntime;
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  installCallback(*game);
  return game;
}

std::unique_ptr<Game> freshGuestTableGame() {
  DmaRuntime &runtime = *guestTableRuntime;
  callbackCalls = 0;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  runtime.registerOverrides(*game);
  installCallback(*game);
  game->hle.i_mask = 1u << IRQ_BIT_DMA;
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
  CHECK_EQ(game->dmaCallbacks.exchange(DmaChannel::Cdrom, kOtherCallback), kCallback);
  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::Cdrom), kOtherCallback);
  CHECK_EQ(game->dmaCallbacks.current(DmaChannel::MdecOut), 0u);
}

void test_runtime_dispatches_registered_completion_once() {
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

// A DMA completion callback is an interrupt handler: the retail exception handler runs it on the
// kernel's exception stack, so it must not push below an interrupted `$sp` that code is using as a
// data pointer. See the same contract for the chain in test_cd_ready_delivery.cpp.
void test_dma_completion_callback_runs_on_the_exception_stack() {
  auto game = freshGame();
  game->dmaCallbacks.exchange(DmaChannel::Cdrom, kCallback);
  constexpr uint32_t kInterruptedSp = 0x8009ABF4u;
  game->core.r[29] = kInterruptedSp;

  completeDma3(*game);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackSp, Hle::kExceptionStackTop);
  CHECK_EQ(game->core.r[29], kInterruptedSp);
}

void test_runtime_without_registration_consumes_without_dispatch() {
  auto game = freshGame();
  completeDma3(*game);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK((game->core.pending_work & Core::PW_IRQ) == 0);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
}

void test_runtime_with_a_declared_guest_table_dispatches_the_guest_word() {
  auto game = freshGuestTableGame();
  game->core.mem_w32(kGuestTable + 4u * static_cast<uint32_t>(DmaChannel::Cdrom), kCallback);
  game->dmaCallbacks.exchange(DmaChannel::Cdrom, kOtherCallback);

  completeDma3(*game);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(callbackCalls, 1);
}

void test_runtime_with_a_declared_guest_table_and_an_empty_slot_delivers_nothing() {
  auto game = freshGuestTableGame();
  game->dmaCallbacks.exchange(DmaChannel::Cdrom, kCallback); // ignored: the guest table is authoritative

  completeDma3(*game);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(callbackCalls, 0);
  CHECK((game->core.pending_work & Core::PW_IRQ) == 0);
}

// The guest's libapi zeroes I_MASK around its GPU queue loops; the callback that would re-enter the
// loop stays owed until the DMA line is unmasked again.
void test_guest_table_callback_waits_for_the_dma_line_to_be_unmasked() {
  auto game = freshGuestTableGame();
  game->core.mem_w32(kGuestTable + 4u * static_cast<uint32_t>(DmaChannel::Cdrom), kCallback);
  game->hle.i_mask = 0u;

  completeDma3(*game);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);

  game->hle.i_mask = 1u << IRQ_BIT_DMA;
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
}

} // namespace

int main() {
  guestTableRuntime = std::make_unique<DmaRuntime>(kGuestTable);
  sharedRuntime = std::make_unique<DmaRuntime>();
  RUN(registration_exchanges_one_typed_channel_entry);
  RUN(runtime_dispatches_registered_completion_once);
  RUN(dma_completion_callback_runs_on_the_exception_stack);
  RUN(runtime_without_registration_consumes_without_dispatch);
  RUN(runtime_with_a_declared_guest_table_dispatches_the_guest_word);
  RUN(runtime_with_a_declared_guest_table_and_an_empty_slot_delivers_nothing);
  RUN(guest_table_callback_waits_for_the_dma_line_to_be_unmasked);
  return psx::test::summary();
}
