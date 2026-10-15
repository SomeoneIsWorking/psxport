// Gate continuous-read callback ownership through the bound StockLibcd's stream pump. Host pacing
// may cap delivery, but it must never invent the controller's INT1 data-ready response.
#include "cdc_state.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "stock_libcd.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kCallbackSlot = 0x80012000u;
constexpr uint32_t kCallback = 0x80012100u;
constexpr uint32_t kVerifier = 0x80012104u;
constexpr uint32_t kInterruptHandler = 0x80012108u;
constexpr uint32_t kInterruptElement = 0x80012200u;
constexpr uint32_t kIStat = 0x1F801070u;
constexpr uint32_t kIMask = 0x1F801074u;

int callbackCalls = 0;
uint32_t callbackA0 = 0;
uint32_t callbackA1 = 0;
int interruptHandlerCalls = 0;
uint8_t interruptResult = 0;

void callback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  callbackA1 = core->r[5];
}

void interruptVerifier(Core *core) {
  core->r[2] = (core->mem_r32(kIStat) & (1u << 2u)) != 0u && core->game->cdc.currentIrqType() == 1u;
}

void interruptHandler(Core *core) {
  ++interruptHandlerCalls;
  psx::cd::CdController &controller = core->game->cdc;
  int previousBank = controller.index;
  controller.write(psx::cd::CdcRegister::Index, 1u);
  interruptResult = static_cast<uint8_t>(controller.read(psx::cd::CdcRegister::Response));
  controller.write(psx::cd::CdcRegister::IrqFlag, 1u);
  controller.write(psx::cd::CdcRegister::Index, static_cast<uint8_t>(previousBank));
  core->mem_w32(kIStat, 0x7FBu);

  uint32_t registered = core->mem_r32(kCallbackSlot);
  if (registered != 0u) {
    core->r[4] = 1u;
    core->r[5] = interruptResult;
    auto result = psx::cpu::dispatchGuest0(*core, registered, psx::cpu::ExecutionBudget::currentTurn(*core));
    CHECK(result.returned());
  }
}

void installCallback(Game &game) {
  auto image = game.core.imageCatalog().activate(
      "test-main", {kCallback & 0x1FFFFFFFu, (kInterruptHandler & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "cd-stream-callback", callback}));
  CHECK(game.core.nativeDispatcher().install({{image, kVerifier}, "cd-interrupt-verifier", interruptVerifier}));
  CHECK(game.core.nativeDispatcher().install({{image, kInterruptHandler}, "cd-interrupt-handler", interruptHandler}));
}

class HostPumpRuntime final : public psx::test::StubGameRuntime {
public:
  void registerOverrides(Game &game) override {
    libcd.bind(game);
  }

  const psx::cd::StockLibcd libcd{{}, {}, kCallbackSlot};
};

class DirectRuntime final : public psx::test::StubGameRuntime {
public:
  void registerOverrides(Game &game) override {
    libcd.bind(game);
  }

  const psx::cd::GuestInterruptLibcd libcd{{}, {}, kCallbackSlot, {}};
};

std::unique_ptr<HostPumpRuntime> hostPumpRuntime;
std::unique_ptr<DirectRuntime> directRuntime;

std::unique_ptr<Game> freshGame() {
  HostPumpRuntime &runtime = *hostPumpRuntime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  runtime.registerOverrides(*game);
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->cd.stream_active = 1;
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  interruptHandlerCalls = 0;
  interruptResult = 0;
  return game;
}

std::unique_ptr<Game> freshInterruptOwnedGame() {
  DirectRuntime &runtime = *directRuntime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  runtime.registerOverrides(*game);
  installCallback(*game);
  game->core.mem_w32(kCallbackSlot, kCallback);
  game->core.mem_w32(kInterruptElement + 4u, kInterruptHandler);
  game->core.mem_w32(kInterruptElement + 8u, kVerifier);
  game->hle.irqEnq(2u, kInterruptElement);
  game->cd.stream_active = 1;
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  interruptHandlerCalls = 0;
  interruptResult = 0;
  return game;
}

void pump(Game &game) {
  game.cd.stockLibrary()->pumpStream(game.core, 1);
}

void publishCurrentResponse(Game &game, uint8_t type) {
  game.cdc.q_head = 0;
  game.cdc.q_tail = 1;
  game.cdc.q[0].type = type;
  game.cdc.q[0].len = 1;
  game.cdc.q[0].resp[0] = game.cdc.stat;
}

void test_empty_controller_queue_does_not_invent_first_callback() {
  auto game = freshGame();

  pump(*game);

  CHECK_EQ(game->cdc.currentIrqType(), 0u);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

void test_non_data_controller_response_is_not_dispatched_as_stream_data() {
  auto game = freshGame();
  publishCurrentResponse(*game, 2u);

  pump(*game);

  CHECK_EQ(game->cdc.currentIrqType(), 2u);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

void test_current_data_ready_response_dispatches_exactly_one_callback() {
  auto game = freshGame();
  publishCurrentResponse(*game, 1u);
  game->core.r[4] = 0xAAAAAAAAu;
  game->core.r[5] = 0xBBBBBBBBu;

  pump(*game);

  CHECK_EQ(game->cdc.currentIrqType(), 1u);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, 1u);
  CHECK_EQ(callbackA1, 0u);
  CHECK_EQ(game->cd.stream_delivered, 1u);
  CHECK_EQ(game->core.r[4], 0xAAAAAAAAu);
  CHECK_EQ(game->core.r[5], 0xBBBBBBBBu);
}

void test_interrupt_owned_stream_waits_for_guest_isr_to_consume_int1() {
  auto game = freshInterruptOwnedGame();
  publishCurrentResponse(*game, 1u);
  game->cdc.stat = 0x22u;
  game->cdc.q[0].resp[0] = 0x22u;
  game->cdc.irq_edge = 1u;
  game->core.mem_w32(kIMask, 1u << 2u);
  game->core.r[4] = 0xAAAAAAAAu;
  game->core.r[5] = 0xBBBBBBBBu;
  game->core.active_native_address = kInterruptHandler;

  pump(*game);
  CHECK_EQ(game->cdc.currentIrqType(), 1u);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(interruptHandlerCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(interruptHandlerCalls, 0);

  game->core.active_native_address = 0u;
  game->hle.irqPoll(&game->core);
  CHECK_EQ(interruptHandlerCalls, 1);
  CHECK_EQ(interruptResult, 0x22u);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, 1u);
  CHECK_EQ(callbackA1, 0x22u);
  CHECK_EQ(game->cdc.currentIrqType(), 0u);
  CHECK_EQ(game->core.mem_r32(kIStat) & (1u << 2u), 0u);
  CHECK_EQ(game->core.r[4], 0xAAAAAAAAu);
  CHECK_EQ(game->core.r[5], 0xBBBBBBBBu);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

void test_interrupt_owned_stream_does_not_dispatch_without_int1() {
  auto game = freshInterruptOwnedGame();
  game->core.mem_w32(kIMask, 1u << 2u);

  pump(*game);
  game->hle.irqPoll(&game->core);

  CHECK_EQ(game->cdc.currentIrqType(), 0u);
  CHECK_EQ(interruptHandlerCalls, 0);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

} // namespace

int main() {
  directRuntime = std::make_unique<DirectRuntime>();
  hostPumpRuntime = std::make_unique<HostPumpRuntime>();
  RUN(empty_controller_queue_does_not_invent_first_callback);
  RUN(non_data_controller_response_is_not_dispatched_as_stream_data);
  RUN(current_data_ready_response_dispatches_exactly_one_callback);
  RUN(interrupt_owned_stream_waits_for_guest_isr_to_consume_int1);
  RUN(interrupt_owned_stream_does_not_dispatch_without_int1);
  return psx::test::summary();
}
