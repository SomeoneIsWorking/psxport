// Host-turn delivery must defer across every guest dispatch critical section without losing work.
#include "game.h"
#include "host_turn.h"
#include "testutil.h"

#include <memory>

namespace {

int deliveredTurns;
bool requestNestedTurn;

void deliverTurn(Core *core) {
  ++deliveredTurns;
  core->r[4] = 0xDEADBEEFu;
  if (requestNestedTurn) {
    requestNestedTurn = false;
    core->pending_work |= Core::PW_HOST;
    psx::cpu::serviceHostTurn(*core);
  }
}

class HostTurnFixture {
public:
  HostTurnFixture() {
    deliveredTurns = 0;
    requestNestedTurn = false;
    game->hle.irq_enabled = 1;
    // No wall-clock event is needed: inject the pending request directly. The timer's first
    // deadline is 1000 seconds away, and shutdown wakes and joins it without waiting for that date.
    psx::cpu::registerHostTurn(game->core, deliverTurn, 1);
    game->core.pending_work = Core::PW_HOST | Core::PW_IRQ;
    game->core.r[4] = 123u;
  }

  ~HostTurnFixture() {
    psx::cpu::shutdownHostTurn();
  }

  std::unique_ptr<Game> game = std::make_unique<Game>();
};

void test_unregistered_request_retires_after_critical_section() {
  auto game = std::make_unique<Game>();
  game->core.pending_work = Core::PW_HOST | Core::PW_IRQ;
  game->hle.irq_enabled = 0;
  psx::cpu::serviceHostTurn(game->core);
  CHECK_EQ(game->core.pending_work, Core::PW_HOST | Core::PW_IRQ);

  game->hle.irq_enabled = 1;
  psx::cpu::serviceHostTurn(game->core);
  CHECK_EQ(game->core.pending_work, Core::PW_IRQ);
}

void test_foreign_core_request_does_not_dispatch_registered_handler() {
  HostTurnFixture fixture;
  auto foreignGame = std::make_unique<Game>();
  foreignGame->hle.irq_enabled = 1;
  foreignGame->core.pending_work = Core::PW_HOST | Core::PW_IRQ;

  psx::cpu::serviceHostTurn(foreignGame->core);

  CHECK_EQ(deliveredTurns, 0);
  CHECK_EQ(foreignGame->core.pending_work, Core::PW_IRQ);
  CHECK_EQ(fixture.game->core.pending_work, Core::PW_HOST | Core::PW_IRQ);

  psx::cpu::serviceHostTurn(fixture.game->core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(fixture.game->core.pending_work, Core::PW_IRQ);
}

void test_irq_handler_defers_until_return_and_preserves_registers() {
  HostTurnFixture fixture;
  auto &game = *fixture.game;
  game.hle.in_irq = 1;
  psx::cpu::serviceHostTurn(game.core);
  CHECK_EQ(deliveredTurns, 0);
  CHECK_EQ(game.core.pending_work, Core::PW_HOST | Core::PW_IRQ);

  game.hle.in_irq = 0;
  psx::cpu::serviceHostTurn(game.core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(game.core.pending_work, Core::PW_IRQ);
  CHECK_EQ(game.core.r[4], 123u);
}

void test_masked_interrupts_preserve_pending_request() {
  HostTurnFixture fixture;
  auto &game = *fixture.game;
  game.hle.irq_enabled = 0;
  psx::cpu::serviceHostTurn(game.core);
  CHECK_EQ(deliveredTurns, 0);
  CHECK_EQ(game.core.pending_work, Core::PW_HOST | Core::PW_IRQ);

  game.hle.irq_enabled = 1;
  psx::cpu::serviceHostTurn(game.core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(game.core.pending_work, Core::PW_IRQ);
}

void test_native_dispatch_preserves_pending_request() {
  HostTurnFixture fixture;
  auto &core = fixture.game->core;
  core.active_native_address = 0x80010000u;
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 0);
  CHECK_EQ(core.pending_work, Core::PW_HOST | Core::PW_IRQ);

  core.active_native_address = 0;
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(core.pending_work, Core::PW_IRQ);
}

void test_pending_redirect_preserves_pending_request() {
  HostTurnFixture fixture;
  auto &core = fixture.game->core;
  core.pending_guest_redirect = 0x80010000u;
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 0);
  CHECK_EQ(core.pending_work, Core::PW_HOST | Core::PW_IRQ);
  CHECK_EQ(core.pending_guest_redirect, 0x80010000u);

  core.pending_guest_redirect = 0;
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(core.pending_work, Core::PW_IRQ);
}

void test_reentrant_delivery_preserves_new_request() {
  HostTurnFixture fixture;
  auto &core = fixture.game->core;
  requestNestedTurn = true;
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(core.pending_work, Core::PW_HOST | Core::PW_IRQ);
  CHECK_EQ(core.r[4], 123u);

  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 2);
  CHECK_EQ(core.pending_work, Core::PW_IRQ);
}

} // namespace

int main() {
  RUN(unregistered_request_retires_after_critical_section);
  RUN(foreign_core_request_does_not_dispatch_registered_handler);
  RUN(irq_handler_defers_until_return_and_preserves_registers);
  RUN(masked_interrupts_preserve_pending_request);
  RUN(native_dispatch_preserves_pending_request);
  RUN(pending_redirect_preserves_pending_request);
  RUN(reentrant_delivery_preserves_new_request);
  return pt_summary();
}
