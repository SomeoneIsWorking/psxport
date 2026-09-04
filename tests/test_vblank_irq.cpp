// The display controller's VBlank interrupt — Timing::raiseVBlank (runtime/psx/timing.cpp).
//
// The port owns frame pacing natively and traps every guest VSync wait; that is unchanged. What is
// asserted here is the separate thing a guest can own itself: the INTERRUPT EDGE. A driver hung off
// the BIOS interrupt chain (Crash Bash's pad engine is one) does its work in a verifier that only
// runs when something raises an interrupt at all.
#include "../runtime/cpu/image_identity.h"
#include "../runtime/cpu/native_dispatch.h"
#include "../runtime/psx/game.h"
#include "../runtime/psx/game_iface.h"
#include "testutil.h"

namespace {

constexpr uint32_t kNtscFieldMilliHz = 59940u;
constexpr uint32_t kIStat = 0x1F801070u;
constexpr uint32_t kIMask = 0x1F801074u;
constexpr uint32_t kElement = 0x80010000u;
constexpr uint32_t kHandler = 0x80010100u;
constexpr uint32_t kVerifier = 0x80010104u;

int verifierCalls = 0;
int handlerCalls = 0;

void verifier(Core *core) {
  ++verifierCalls;
  core->r[2] = (core->mem_r32(kIStat) & 1u) != 0 ? 1u : 0u;
}

void handler(Core *core) {
  ++handlerCalls;
  core->mem_w32(kIStat, 0x7FEu); // handler acknowledges VBlank by writing bit 0 as zero
}

void installInterruptCallbacks(Game &game) {
  const auto image =
      game.core.imageCatalog().activate("test-main", {kElement & 0x1FFFFFFFu, (kVerifier & 0x1FFFFFFFu) + 4u}, 1u);
  CHECK(game.core.nativeDispatcher().install({{image, kHandler}, "vblank-handler", handler}));
  CHECK(game.core.nativeDispatcher().install({{image, kVerifier}, "vblank-verifier", verifier}));
}

Game *interruptGame() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.recMainLo = kHandler & 0x1FFFFFFFu;
  config.recMainHi = (kVerifier & 0x1FFFFFFFu) + 4u;
  psxport_install_game(&config, &hooks);
  verifierCalls = 0;
  handlerCalls = 0;
  Game *game = new Game();
  installInterruptCallbacks(*game);
  game->core.mem_w32(kElement + 4u, kHandler);
  game->core.mem_w32(kElement + 8u, kVerifier);
  game->hle.irqEnq(2, kElement);
  return game;
}

void test_a_field_advance_latches_i_stat_bit0() {
  Game *game = new Game();
  game->hle.i_stat = 0;
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 1u);
  delete game;
}

// It is a LATCH, not a counter: a guest that has not acknowledged sees one pending VBlank, exactly
// like the hardware bit. Two fields with no ack in between are still one set bit.
void test_the_bit_latches_and_survives_until_acknowledged() {
  Game *game = new Game();
  game->hle.i_stat = 0;
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat, 1u);
  game->hle.i_stat &= ~1u; // the guest's own I_STAT write-0 acknowledge
  CHECK_EQ(game->hle.i_stat & 1u, 0u);
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 1u);
  delete game;
}

// Delivery is armed, because a latched bit nobody looks at is the same as no bit at all.
void test_the_delivery_gate_is_armed() {
  Game *game = new Game();
  game->core.pending_work = 0;
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  CHECK_EQ(game->core.pending_work & Core::PW_IRQ, (uint32_t)Core::PW_IRQ);
  delete game;
}

// fps60 presents twice per guest field and paces each presentation by one half. The first
// presentation is not a physical VBlank; the second completes the field and raises exactly one.
void test_two_half_field_paces_raise_one_vblank() {
  Game *game = new Game();
  game->hle.i_stat = 0;
  CHECK(game->timing.advanceDisplayFields(1, 2, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 0u);
  CHECK(game->timing.advanceDisplayFields(1, 2, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 1u);
  delete game;
}

// A two-field title split into two presentations still completes one physical field per call.
void test_two_field_quota_split_in_half_raises_each_call() {
  Game *game = new Game();
  game->hle.i_stat = 0;
  CHECK(game->timing.advanceDisplayFields(2, 2, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 1u);
  game->hle.i_stat &= ~1u;
  CHECK(game->timing.advanceDisplayFields(2, 2, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 1u);
  delete game;
}

// The negative: an advance that does NOT happen raises nothing. Both refusal shapes are covered so
// a "no VBlank" result cannot come from the method silently doing nothing at all.
void test_a_refused_advance_raises_nothing() {
  Game *game = new Game();
  game->hle.i_stat = 0;
  CHECK(!game->timing.advanceDisplayFields(0, 1, kNtscFieldMilliHz));
  CHECK(!game->timing.advanceDisplayFields(1, 1, 0));
  CHECK(!game->timing.advanceDisplayFields(1, 0, kNtscFieldMilliHz));
  CHECK_EQ(game->hle.i_stat & 1u, 0u);
  delete game;
}

// The complete registered-chain boundary: a masked VBlank stays latched without running guest code;
// writing I_MASK through guest MMIO arms that same edge, whose verifier and handler each run once.
void test_masked_edge_dispatches_once_after_guest_unmasks_it() {
  Game *game = interruptGame();
  game->core.mem_w32(kIMask, 0);
  CHECK(game->timing.advanceDisplayFields(1, 1, kNtscFieldMilliHz));
  game->hle.irqPoll(&game->core);
  CHECK_EQ(verifierCalls, 0);
  CHECK_EQ(handlerCalls, 0);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 1u);

  game->core.mem_w32(kIMask, 1u);
  CHECK((game->core.pending_work & Core::PW_IRQ) != 0);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(verifierCalls, 1);
  CHECK_EQ(handlerCalls, 1);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 0u);
  game->hle.irqPoll(&game->core);
  CHECK_EQ(verifierCalls, 1);
  CHECK_EQ(handlerCalls, 1);
  delete game;
}

} // namespace

int main() {
  RUN(a_field_advance_latches_i_stat_bit0);
  RUN(the_bit_latches_and_survives_until_acknowledged);
  RUN(the_delivery_gate_is_armed);
  RUN(two_half_field_paces_raise_one_vblank);
  RUN(two_field_quota_split_in_half_raises_each_call);
  RUN(a_refused_advance_raises_nothing);
  RUN(masked_edge_dispatches_once_after_guest_unmasks_it);
  return pt_summary();
}
