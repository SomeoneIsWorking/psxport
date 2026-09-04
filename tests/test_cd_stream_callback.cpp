// Gate continuous-read callback ownership through the shipping Cd::pumpStream path. Host pacing
// may cap delivery, but it must never invent the controller's INT1 data-ready response.
#include "cdc_state.h"
#include "game.h"
#include "game_iface.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kCallbackSlot = 0x80012000u;
constexpr uint32_t kCallback = 0x80012100u;

int callbackCalls = 0;
uint32_t callbackA0 = 0;
uint32_t callbackA1 = 0;

void callback(Core *core) {
  ++callbackCalls;
  callbackA0 = core->r[4];
  callbackA1 = core->r[5];
}

void installCallback(Game &game) {
  const auto image =
      game.core.imageCatalog().activate("test-main", {kCallback & 0x1FFFFFFFu, (kCallback & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kCallback}, "cd-stream-callback", callback}));
}

std::unique_ptr<Game> freshGame() {
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
  callbackCalls = 0;
  callbackA0 = 0;
  callbackA1 = 0;
  return game;
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

  game->cd.pumpStream(&game->core, 1);

  CHECK_EQ(cdc_current_irq_type(&game->cdc), 0u);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

void test_non_data_controller_response_is_not_dispatched_as_stream_data() {
  auto game = freshGame();
  publishCurrentResponse(*game, 2u);

  game->cd.pumpStream(&game->core, 1);

  CHECK_EQ(cdc_current_irq_type(&game->cdc), 2u);
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(game->cd.stream_delivered, 0u);
}

void test_current_data_ready_response_dispatches_exactly_one_callback() {
  auto game = freshGame();
  publishCurrentResponse(*game, 1u);
  game->core.r[4] = 0xAAAAAAAAu;
  game->core.r[5] = 0xBBBBBBBBu;

  game->cd.pumpStream(&game->core, 1);

  CHECK_EQ(cdc_current_irq_type(&game->cdc), 1u);
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(callbackA0, 1u);
  CHECK_EQ(callbackA1, 0u);
  CHECK_EQ(game->cd.stream_delivered, 1u);
  CHECK_EQ(game->core.r[4], 0xAAAAAAAAu);
  CHECK_EQ(game->core.r[5], 0xBBBBBBBBu);
}

} // namespace

int main() {
  RUN(empty_controller_queue_does_not_invent_first_callback);
  RUN(non_data_controller_response_is_not_dispatched_as_stream_data);
  RUN(current_data_ready_response_dispatches_exactly_one_callback);
  return pt_summary();
}
