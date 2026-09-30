// The framework's own CD-ROM interrupt handler (runtime/psx/cd_ready_delivery.*).
//
// WHAT IS ASSERTED, and why a test of only this path would prove nothing. The framework now stands
// in for the BIOS's own CD-ROM interrupt handler, which is ROM code this port does not have, and
// dispatches a title's registered stock-libcd ready callback when that title declares
// `GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt`. Every one of those cases is the
// NEGATIVE of a control:
//
//   * the `HostPump` owner must still receive EXACTLY ONE callback per completion, and it must come
//     from the host pump and not from the interrupt arm. A test that only exercised the new path
//     would pass identically for a framework that delivered every completion twice;
//   * a guest whose own registered interrupt element claims the CD bit keeps that one delivery, and
//     the framework adds none;
//   * a completion the arm cannot deliver is left OWED, because these completions chain and a
//     consumed-but-undelivered one ends the chain — the defect the DMA arm's own comments record.
#include "cd_control.h"
#include "cd_ready_delivery.h"
#include "cd_stock_read_completion.h"
#include "cdc_state.h"
#include "core.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "guest_cd_stream_callback_layout.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>
#include <vector>

namespace {

constexpr uint32_t kCallbackSlot = 0x80012000u;
constexpr uint32_t kCallback = 0x80012100u;
constexpr uint32_t kVerifier = 0x80012104u;
constexpr uint32_t kInterruptHandler = 0x80012108u;
constexpr uint32_t kBareInterruptHandler = 0x80012110u;
constexpr uint32_t kChainCallback = 0x80012114u;
constexpr uint32_t kInterruptElement = 0x80012200u;
constexpr uint32_t kIStat = 0x1F801070u;
constexpr uint32_t kIMask = 0x1F801074u;
constexpr uint32_t kCdcIndex = 0x1F801800u;

int callbackCalls = 0;
uint32_t callbackA0 = 0;
uint32_t callbackA1 = 0;
int interruptHandlerCalls = 0;
int verifierCalls = 0;
uint8_t interruptResult = 0;

// The guest's own ready callback. Records the arguments the framework delivered, and — the chain —
// issues the next request from inside itself, so the response for it becomes current BEFORE the
// callback returns. That is the case a consumed-but-deferred completion loses.
void callback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  callbackA1 = core->r[5];
}

// A verifier that claims only the CD bit, the shape a guest's own CD service would have.
void interruptVerifier(Core *core) {
  ++verifierCalls;
  core->r[2] = (core->mem_r32(kIStat) & (1u << 2u)) != 0u && cdc_current_irq_type(&core->game->cdc) == 1u;
}

// A guest that services the CD-ROM itself: consume the controller response, acknowledge, and call
// whatever is in the slot. This is the shape of the guest ISR at 0x8008C3E0 in SLUS_008.75, and the
// reason the framework's arm must stay out of its way.
void interruptHandler(Core *core) {
  ++interruptHandlerCalls;
  CdcState &controller = core->game->cdc;
  const int previousBank = controller.index;
  cdc_write(&controller, 0u, 1u);
  interruptResult = static_cast<uint8_t>(cdc_read(&controller, 1u));
  cdc_write(&controller, 3u, 1u);
  cdc_write(&controller, 0u, static_cast<uint8_t>(previousBank));
  core->mem_w32(kIStat, 0x7FBu);
  const uint32_t registered = core->mem_r32(kCallbackSlot);
  if (registered != 0u) {
    core->r[4] = 1u;
    core->r[5] = interruptResult;
    CHECK(psx::cpu::dispatchGuest0(*core, registered, psx::cpu::ExecutionBudget::currentTurn(*core)).returned());
  }
}

// A guest element that CLAIMS the CD interrupt and then does nothing at all: it does not consume the
// controller response and it does not acknowledge I_STAT. On hardware the first matching entry runs
// and the BIOS's built-in CD entry does not, so the completion must stay in the controller even
// though nobody took it. This is the case that distinguishes "the guest's element ran" from "the
// completion was already taken" -- a handler that acknowledged would make the framework's arm look
// correct for the wrong reason.
void bareInterruptHandler(Core *) {
  ++interruptHandlerCalls;
}

constexpr uint32_t kReadBuffer = 0x80130000u;
int chainRemaining = 0;

// The guest's ready callback of a chained loader: on every completion it starts the NEXT read, until the
// module is done. It runs with `in_irq` set, so the read it issues queues a completion that must stay owed
// until this callback has returned.
void chainingCallback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  if (chainRemaining > 0) {
    --chainRemaining;
    core->game->cd.setloc_lba = 100;
    core->r[4] = 1;
    core->r[5] = kReadBuffer;
    core->r[6] = 0x80u;
    cd_read_stock_sync(core);
  }
}

void installCallback(Game &game) {
  const auto image = game.core.imageCatalog().activate(
      "test-main", {kCallback & 0x1FFFFFFFu, (kChainCallback & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "cd-ready-callback", callback}));
  CHECK(game.core.nativeDispatcher().install({{image, kVerifier}, "cd-interrupt-verifier", interruptVerifier}));
  CHECK(game.core.nativeDispatcher().install({{image, kInterruptHandler}, "cd-interrupt-handler", interruptHandler}));
  CHECK(
      game.core.nativeDispatcher().install({{image, kBareInterruptHandler}, "cd-bare-handler", bareInterruptHandler}));
  CHECK(game.core.nativeDispatcher().install({{image, kChainCallback}, "cd-chain-callback", chainingCallback}));
}

class DirectRuntime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  const GuestCdStreamCallbackLayout *guestCdStreamCallbackLayout() const override {
    return &callbacks;
  }

  void stockCdReadLanded(Core &, const psx::cd::StockReadLanding &landing) override {
    landings.push_back(landing);
  }

  void select(GuestCdStreamCallbackLayout::DeliveryOwner owner) {
    callbacks.owner = owner;
  }
  void dropSlot() {
    callbacks.readyCallbackPointer = 0;
  }

  GuestCdStreamCallbackLayout callbacks{kCallbackSlot, GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt};
  std::vector<psx::cd::StockReadLanding> landings;
};

DirectRuntime &runtime() {
  static DirectRuntime instance;
  return instance;
}

void resetCounters() {
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  interruptHandlerCalls = 0;
  verifierCalls = 0;
  interruptResult = 0;
}

std::unique_ptr<Game> freshDirectGame() {
  runtime().landings.clear();
  runtime().select(GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt);
  runtime().dropSlot();
  runtime().callbacks.readyCallbackPointer = kCallbackSlot;
  runtime().callbacks.readyStatus = 1;
  runtime().callbacks.stockReadRaisesCompletion = false;
  psxport_install_game(runtime());
  auto game = std::make_unique<Game>();
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->cd.stream_active = 1;
  resetCounters();
  return game;
}

// A legacy GameConfig consumer: it has no declaration to honour, so it keeps host dispatch. Its
// ready-callback slot lives in the same RAM the direct fixture uses, which is what makes this a
// control on the DECLARATION rather than on the address.
std::unique_ptr<Game> freshLegacyGame() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.recMainLo = kCallback & 0x1FFFFFFFu;
  config.recMainHi = (kCallback & 0x1FFFFFFFu) + 4u;
  config.cdReadyCbPtr = kCallbackSlot;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->cd.stream_active = 1;
  resetCounters();
  return game;
}

// Queue `count` data-ready responses the way the controller does, the last of which becomes current.
void queueDataReady(Game &game, int count) {
  CdcState &controller = game.cdc;
  for (int i = 0; i < count; i++) {
    const int at = (controller.q_tail + 1) & 7;
    controller.q[controller.q_tail].type = 1u;
    controller.q[controller.q_tail].len = 1;
    controller.q[controller.q_tail].resp[0] = static_cast<uint8_t>(0x20u + i);
    controller.q_tail = at;
  }
  controller.resp_rd = 0;
  controller.irq_edge = 1u; // -> I_STAT bit 2, latched by Core::irqStatLatch
}

void queueResponse(Game &game, uint8_t type) {
  CdcState &controller = game.cdc;
  const int at = (controller.q_tail + 1) & 7;
  controller.q[controller.q_tail].type = type;
  controller.q[controller.q_tail].len = 1;
  controller.q[controller.q_tail].resp[0] = 0x22u;
  controller.q_tail = at;
  controller.resp_rd = 0;
  controller.irq_edge = 1u;
}

void armCdLine(Game &game) {
  game.core.mem_w32(kIMask, 1u << 2u);
  game.core.pending_work |= Core::PW_IRQ;
}

// The controller raising an edge is not the same instant as I_STAT showing it: the fold happens on
// the next I_STAT access, which is what `Core::irqStatLatch()` exists for and what `Hle::irqPoll`
// calls before it tests the pending word. A fixture that queued a response and delivered without
// this step would be testing a line the framework had not yet been told about.
void latchCdLine(Game &game) {
  game.core.irqStatLatch();
}

// THE POSITIVE CASE: the declared owner gets its completion at the interrupt, once, with the response
// consumed and the register context restored around it.
void test_guest_interrupt_owner_receives_the_completion() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, 1u);
  CHECK_EQ(callbackA1, 0u);
  // The handler consumed the response, and the CD line it serviced is acknowledged.
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  CHECK_EQ(game->hle.i_stat & (1u << 2u), 0u);
  CHECK_EQ(game->hle.cd_ready_delivered, 1u);
  CHECK_EQ(game->hle.cd_ready_declined, 0u);
  // The controller's register bank is left as it was found; a handler that leaves the guest's
  // controller mid-transaction corrupts every later read.
  CHECK_EQ(game->cdc.index, 0);
  delete game.release();
}

// Exactly ONCE, and that is the claim: a second poll with nothing new owed must not re-deliver, or
// the callback runs twice for one sector and the stream's own counter overshoots.
void test_one_completion_is_delivered_exactly_once() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  game->hle.irqPoll(&game->core);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(game->hle.cd_ready_delivered, 1u);
  delete game.release();
}

// THE CONTROL. Same fixture, same slot, same completion — only the DECLARED OWNER differs, and the
// answer must be the host pump's callback, not the interrupt's. This is the case a test of only the
// new path cannot see. The check order is the point: the arm's effect is measured as a CALLBACK
// COUNT through the shipping poll, before the host pump ever runs.
void test_host_pump_owner_gets_exactly_one_callback_from_the_pump() {
  runtime().select(GuestCdStreamCallbackLayout::DeliveryOwner::HostPump);
  runtime().callbacks.readyCallbackPointer = kCallbackSlot;
  psxport_install_game(runtime());
  auto game = std::make_unique<Game>();
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->cd.stream_active = 1;
  resetCounters();
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  // The interrupt arm must do NOTHING AT ALL, measured as a callback count before the pump ever
  // runs. A framework that delivered every completion twice fails here with the number 1, the
  // second delivery, rather than with a shape mismatch.
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(interruptHandlerCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u); // the completion is still owed
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);     // ...and the arm never ran

  // The host pump delivers it, and delivers it once.
  game->cd.pumpStream(&game->core, 1);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(interruptHandlerCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 1u);

  // A further poll and a further pump add nothing: one completion, one callback.
  game->cd.pumpStream(&game->core, 1);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(game->hle.cd_ready_delivered, 0u); // the interrupt arm never ran at all
  delete game.release();
}

// The same control read through the typed entry points, so a reader can see the refusal is the
// DECLARATION and not a side effect of the poll.
void test_the_owner_declaration_is_the_gate() {
  runtime().select(GuestCdStreamCallbackLayout::DeliveryOwner::HostPump);
  runtime().callbacks.readyCallbackPointer = kCallbackSlot;
  psxport_install_game(runtime());
  auto hostPump = std::make_unique<Game>();
  CHECK_EQ(cdReadyCallbackOwnedByGuestInterrupt(hostPump->core), false);
  CHECK_EQ(cdReadyCompletionOwed(hostPump->core), false);

  runtime().select(GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt);
  auto guestInterrupt = std::make_unique<Game>();
  CHECK_EQ(cdReadyCallbackOwnedByGuestInterrupt(guestInterrupt->core), true);
  armCdLine(*guestInterrupt);
  queueDataReady(*guestInterrupt, 1);
  latchCdLine(*guestInterrupt);
  CHECK_EQ(cdReadyCompletionOwed(guestInterrupt->core), true);
  delete hostPump.release();
  delete guestInterrupt.release();
}

// The OTHER control: a legacy GameConfig consumer declares nothing and keeps host dispatch, even
// though the same slot holds the same callback.
void test_legacy_consumer_is_not_owned_by_the_interrupt() {
  auto game = freshLegacyGame();
  CHECK_EQ(cdReadyCallbackOwnedByGuestInterrupt(game->core), false);
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NotOwned);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);
  delete game.release();
}

// A guest that services CD-ROM through its OWN registered element keeps exactly that one delivery.
// The framework's arm is the BIOS's BUILT-IN entry, and the guest's element is first refusal.
void test_guest_element_keeps_its_own_single_delivery() {
  auto game = freshDirectGame();
  game->core.mem_w32(kInterruptElement + 4u, kInterruptHandler);
  game->core.mem_w32(kInterruptElement + 8u, kVerifier);
  game->hle.irqEnq(2u, kInterruptElement);
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  game->hle.irqPoll(&game->core);

  CHECK_EQ(verifierCalls, 1);
  CHECK_EQ(interruptHandlerCalls, 1);
  CHECK_EQ(interruptResult, 0x20u);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA1, 0x20u); // the guest's own ISR staged the response byte, not the framework
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);
  delete game.release();
}

// THE ORDER GATE, on its own: the guest's element claimed the interrupt, so the framework's built-in
// CD entry does not run -- even with the completion still sitting in the controller. Removing the
// `!claimed` condition from `Hle::irqPoll` delivers a second time here, and the test says so.
void test_a_claiming_guest_element_still_blocks_the_framework_arm() {
  auto game = freshDirectGame();
  game->core.mem_w32(kInterruptElement + 4u, kBareInterruptHandler);
  game->core.mem_w32(kInterruptElement + 8u, kVerifier);
  game->hle.irqEnq(2u, kInterruptElement);
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  game->hle.irqPoll(&game->core);

  CHECK_EQ(verifierCalls, 1);
  CHECK_EQ(interruptHandlerCalls, 1);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u); // untouched, because the guest took the interrupt
  delete game.release();
}

// A masked-off interrupt is not delivered, and the completion stays in the controller. This is the
// hardware's own gate; a handler that ignored it would call a callback the guest disabled.
void test_masked_cd_line_owes_the_completion() {
  auto game = freshDirectGame();
  game->core.mem_w32(kIMask, 0u);
  queueDataReady(*game, 1);
  latchCdLine(*game);

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NothingOwed);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);
  CHECK_EQ(game->hle.cd_ready_declined, 0u); // not owed is not a declined delivery

  game->core.mem_w32(kIMask, 1u << 2u);
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 1);
  delete game.release();
}

// Only the DATA-READY response is a ready-callback completion. A command-acknowledge or
// command-complete belongs to the title's own libcd command state machine.
void test_a_non_data_ready_response_is_not_a_ready_completion() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueResponse(*game, 3u);
  latchCdLine(*game);

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NothingOwed);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 3u);
  delete game.release();
}

// A completion with nothing installed to receive it is left owed, not consumed: the guest installs
// its callback during CdInit and the first INT1 can arrive on the boundary.
void test_no_installed_callback_leaves_the_completion_owed() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);
  game->core.mem_w32(kCallbackSlot, 0u);

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NothingOwed);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);
  CHECK_EQ(game->hle.i_stat & (1u << 2u), 0x4u); // the line is still asserted; nobody serviced it
  CHECK_EQ(game->hle.cd_ready_declined, 1u);

  game->core.mem_w32(kCallbackSlot, kCallback);
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 1);
  delete game.release();
}

// A guest ready callback's own polls re-enter the CD owner, and the delivery must not re-enter
// itself. The second sector's completion is consumed by the first callback's window, so it has to
// stay owed for the next poll to find.
void test_reentrant_poll_defers_and_the_chain_continues() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 2);
  latchCdLine(*game);
  game->core.r[4] = 0xAAAAAAAAu;
  game->core.r[5] = 0xBBBBBBBBu;

  // Stand in for a poll taken from inside the ready callback: `in_irq` is exactly what a guest
  // callback's own function-entry poll sees.
  game->hle.in_irq = 1;
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NothingOwed);
  game->hle.in_irq = 0;
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->hle.cd_ready_declined, 1u);

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(game->cdc.index, 0);
  // The second completion is STILL owed — the chain is intact, not truncated.
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 2);
  CHECK_EQ(game->hle.cd_ready_delivered, 2u);
  CHECK_EQ(game->hle.cd_ready_declined, 1u);
  delete game.release();
}

// THE CHAIN, through the shipping poll: two queued completions and two polls deliver two callbacks,
// and the second is the one a take-first design would have dropped.
void test_two_queued_completions_deliver_two_callbacks_across_polls() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 2);
  latchCdLine(*game);

  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u); // the queued second response became current
  CHECK_EQ(game->hle.i_stat & (1u << 2u), 0x4u);  // ...and raised a FRESH line, as hardware does
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 2);
  CHECK_EQ(game->hle.cd_ready_delivered, 2u);
  delete game.release();
}

// The delivery runs at a guest FUNCTION-ENTRY boundary, never from inside a native override. A poll
// taken while a native override is active finds nothing to deliver, and the completion is untouched.
void test_delivery_waits_for_a_call_coherent_boundary() {
  auto game = freshDirectGame();
  armCdLine(*game);
  queueDataReady(*game, 1);
  latchCdLine(*game);
  game->core.active_native_address = kCallback;

  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::NothingOwed);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);

  game->core.active_native_address = 0u;
  CHECK_EQ(deliverCdReadyCompletionOnInterrupt(game->core), CdReadyDelivery::Delivered);
  CHECK_EQ(callbackCalls, 1);
  delete game.release();
}

// ----------------------------------------------------------------------------
// The completion a SYNCHRONOUS stock CdRead owes a guest that chains reads from its ready callback
// (runtime/psx/cd_stock_read_completion.*). These drive the shipping `cd_read_stock_sync` with fake
// sectors through the controller's sector-source binding, and read the answer as a CALLBACK COUNT through
// the shipping poll. Every positive has a negative beside it, because a count of 0 is also what a
// framework that never raised anything would print.

constexpr uint32_t kCdlCompleteCode = 2u;
int sectorsServed = 0;

int fakeSector(DiscState *, uint32_t lba, uint8_t *out, uint32_t) {
  ++sectorsServed;
  for (uint32_t i = 0; i < 2352u; i++) {
    out[i] = static_cast<uint8_t>(lba + i);
  }
  return 1;
}

// One CdRead of `sectors` sectors, positioned, through the shipping stock owner.
void stockRead(Game &game, uint32_t sectors) {
  game.cd.setloc_lba = 100;
  game.core.r[4] = sectors;
  game.core.r[5] = kReadBuffer;
  game.core.r[6] = 0x80u;
  cd_read_stock_sync(&game.core);
  CHECK_EQ(game.core.r[2], 1u);
}

std::unique_ptr<Game> freshStockReadGame(bool declareCompletion, uint8_t status) {
  auto game = freshDirectGame();
  runtime().callbacks.readyStatus = status;
  runtime().callbacks.stockReadRaisesCompletion = declareCompletion;
  game->cdc.disc_read_raw_fn = fakeSector;
  sectorsServed = 0;
  armCdLine(*game);
  return game;
}

// THE POSITIVE: N stock reads, N deliveries, each with the declared completion code in $a0, the
// controller empty afterwards.
void test_declared_guest_interrupt_gets_one_delivery_per_stock_read() {
  constexpr int kReads = 5;
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  for (int i = 0; i < kReads; i++) {
    stockRead(*game, 1);
    game->hle.irqPoll(&game->core);
    game->hle.irqPoll(&game->core); // a second poll must add nothing
    CHECK_EQ(callbackCalls, i + 1);
  }
  CHECK_EQ(sectorsServed, kReads);
  CHECK_EQ(callbackA0, kCdlCompleteCode);
  CHECK_EQ(game->hle.cd_ready_delivered, static_cast<uint32_t>(kReads));
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  CHECK_EQ(game->hle.i_stat & (1u << 2u), 0u);
  delete game.release();
}

// The line is opened by whoever replaces CdInit, and only by them: a stock read on a masked CD line leaves
// its completion OWED (not dropped, not delivered), and arming the line through `armCdInterrupt` delivers
// it exactly once while keeping every other enable the guest had set. This is the Spyro 2 shape, measured:
// I_MASK 0x009 at the first stock read.
void test_a_masked_cd_line_leaves_the_stock_completion_owed_until_armed() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  game->core.mem_w32(kIMask, 0x009u);
  stockRead(*game, 1);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u); // still owed

  CHECK_EQ(psx::cd::armCdInterrupt(game->core), true);
  CHECK_EQ(game->core.mem_r32(kIMask), 0x00Du);         // the guest's own bits survive
  CHECK_EQ(psx::cd::armCdInterrupt(game->core), false); // idempotent
  game->hle.irqPoll(&game->core);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, kCdlCompleteCode);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  delete game.release();
}

// A multi-sector read is ONE read and owes ONE completion, however many sectors it moved.
void test_a_multi_sector_read_owes_one_completion() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  stockRead(*game, 7);
  CHECK_EQ(sectorsServed, 7);
  game->hle.irqPoll(&game->core);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(game->hle.cd_ready_delivered, 1u);
  delete game.release();
}

// NEGATIVE 1: a GuestInterrupt title that does not opt in (Spider-Man 1's shape) gets nothing, and its
// controller is untouched.
void test_guest_interrupt_without_the_declaration_gets_no_completion() {
  auto game = freshStockReadGame(false, 1);
  stockRead(*game, 1);
  CHECK_EQ(sectorsServed, 1); // the read itself still happened
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);
  CHECK_EQ(psx::cd::stockReadOwesCompletion(game->core), false);
  delete game.release();
}

// NEGATIVE 2: the flag means nothing under the host-pump owner, which keeps its own single delivery.
void test_host_pump_owner_gets_no_stock_read_completion() {
  runtime().select(GuestCdStreamCallbackLayout::DeliveryOwner::HostPump);
  runtime().callbacks.readyCallbackPointer = kCallbackSlot;
  runtime().callbacks.stockReadRaisesCompletion = true;
  psxport_install_game(runtime());
  auto game = std::make_unique<Game>();
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->cdc.disc_read_raw_fn = fakeSector;
  resetCounters();
  armCdLine(*game);
  stockRead(*game, 1);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  CHECK_EQ(game->hle.cd_ready_delivered, 0u);
  runtime().callbacks.stockReadRaisesCompletion = false;
  delete game.release();
}

// NEGATIVE 3: a legacy GameConfig consumer declares nothing.
void test_legacy_consumer_gets_no_stock_read_completion() {
  auto game = freshLegacyGame();
  game->cdc.disc_read_raw_fn = fakeSector;
  armCdLine(*game);
  stockRead(*game, 1);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  delete game.release();
}

// NEGATIVE 4: a read that failed, or moved no sector, completed nothing.
void test_a_failed_or_empty_read_owes_nothing() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  game->cd.setloc_lba = 100;
  game->core.r[4] = 0;
  game->core.r[5] = kReadBuffer;
  game->core.r[6] = 0x80u;
  cd_read_stock_sync(&game->core); // zero sectors
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);

  game->cdc.disc_read_raw_fn = [](DiscState *, uint32_t, uint8_t *, uint32_t) -> int {
    return 0;
  };
  game->core.r[4] = 1;
  cd_read_stock_sync(&game->core);
  CHECK_EQ(game->core.r[2], 0u);
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  delete game.release();
}

// A completion that cannot be queued is reported, not counted: nothing is posted and nothing is owed.
void test_a_full_controller_queue_refuses_the_completion() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  int posted = 0;
  while (psx::cd::raiseStockReadCompletion(game->core, 1)) {
    ++posted;
  }
  CHECK_EQ(posted, 7); // the 8-entry ring holds 7
  CHECK_EQ(psx::cd::raiseStockReadCompletion(game->core, 1), false);
  delete game.release();
}

// THE CHAIN, which is the defect: the loader issues ONE read and every further read is started from the
// callback. One initial read plus a callback that starts `kFollowers` more must yield exactly
// 1 + kFollowers reads and deliveries, and then stop: no delivery is lost behind the in_irq deferral and
// none is repeated.
void test_a_chained_loader_completes_exactly_its_reads() {
  constexpr int kFollowers = 4;
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  game->core.mem_w32(kCallbackSlot, kChainCallback);
  chainRemaining = kFollowers;
  stockRead(*game, 1);
  for (int poll = 0; poll < 32; poll++) {
    game->hle.irqPoll(&game->core);
  }
  CHECK_EQ(sectorsServed, 1 + kFollowers);
  CHECK_EQ(callbackCalls, 1 + kFollowers);
  CHECK_EQ(callbackA0, kCdlCompleteCode);
  CHECK_EQ(chainRemaining, 0);
  CHECK_EQ(game->hle.cd_ready_delivered, static_cast<uint32_t>(1 + kFollowers));
  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  delete game.release();
}

// THE LANDING ANNOUNCEMENT. A runtime that publishes a streamed region as a code image must be told
// exactly once per whole read, with what landed and where; each negative below is a read that landed
// nothing a runtime could name, and a recorder that never ran would also print zero landings, so the
// positive case comes first and the recorder is shown firing.
void test_a_whole_read_announces_its_landing_once_with_exact_bytes() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  stockRead(*game, 3);
  CHECK_EQ(runtime().landings.size(), 1u);
  CHECK_EQ(runtime().landings[0].firstLba, 100u);
  CHECK_EQ(runtime().landings[0].sectors, 3u);
  CHECK_EQ(runtime().landings[0].destination, kReadBuffer);
  CHECK_EQ(runtime().landings[0].bytes, 3u * 2048u);
  // The announcement comes after the bytes are visible: the last payload byte is already in RAM.
  CHECK_EQ(game->core.mem_r8(kReadBuffer + 3u * 2048u - 1u), static_cast<uint8_t>(102u + 24u + 2047u));
  stockRead(*game, 1);
  CHECK_EQ(runtime().landings.size(), 2u);
  CHECK_EQ(runtime().landings[1].firstLba, 100u);
  delete game.release();
}

void test_a_failed_or_empty_read_announces_no_landing() {
  auto game = freshStockReadGame(true, kCdlCompleteCode);
  game->cd.setloc_lba = 100;
  game->core.r[4] = 0;
  game->core.r[5] = kReadBuffer;
  game->core.r[6] = 0x80u;
  cd_read_stock_sync(&game->core); // zero sectors
  CHECK_EQ(runtime().landings.size(), 0u);

  game->cdc.disc_read_raw_fn = [](DiscState *, uint32_t lba, uint8_t *out, uint32_t) -> int {
    return lba == 100u ? fakeSector(nullptr, lba, out, 0u) : 0; // the second sector is unreadable
  };
  game->core.r[4] = 2;
  cd_read_stock_sync(&game->core);
  CHECK_EQ(game->core.r[2], 0u);
  CHECK_EQ(runtime().landings.size(), 0u);

  game->cd.setloc_lba = -1; // never positioned: refused before any byte is read
  game->core.r[4] = 1;
  cd_read_stock_sync(&game->core);
  CHECK_EQ(runtime().landings.size(), 0u);
  delete game.release();
}

} // namespace

int main() {
  RUN(guest_interrupt_owner_receives_the_completion);
  RUN(one_completion_is_delivered_exactly_once);
  RUN(host_pump_owner_gets_exactly_one_callback_from_the_pump);
  RUN(the_owner_declaration_is_the_gate);
  RUN(legacy_consumer_is_not_owned_by_the_interrupt);
  RUN(guest_element_keeps_its_own_single_delivery);
  RUN(a_claiming_guest_element_still_blocks_the_framework_arm);
  RUN(masked_cd_line_owes_the_completion);
  RUN(a_non_data_ready_response_is_not_a_ready_completion);
  RUN(no_installed_callback_leaves_the_completion_owed);
  RUN(reentrant_poll_defers_and_the_chain_continues);
  RUN(two_queued_completions_deliver_two_callbacks_across_polls);
  RUN(delivery_waits_for_a_call_coherent_boundary);
  RUN(declared_guest_interrupt_gets_one_delivery_per_stock_read);
  RUN(a_masked_cd_line_leaves_the_stock_completion_owed_until_armed);
  RUN(a_multi_sector_read_owes_one_completion);
  RUN(guest_interrupt_without_the_declaration_gets_no_completion);
  RUN(host_pump_owner_gets_no_stock_read_completion);
  RUN(legacy_consumer_gets_no_stock_read_completion);
  RUN(a_failed_or_empty_read_owes_nothing);
  RUN(a_full_controller_queue_refuses_the_completion);
  RUN(a_chained_loader_completes_exactly_its_reads);
  RUN(a_whole_read_announces_its_landing_once_with_exact_bytes);
  RUN(a_failed_or_empty_read_announces_no_landing);
  return pt_summary();
}
