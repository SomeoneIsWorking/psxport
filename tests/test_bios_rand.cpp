// BIOS A0:0x2F/0x30 rand/srand must be deterministic, per-Game, and reached
// through the same Hle::dispatchBios seam as the guest's tail-jump stubs. The
// sequence is Sony's libc LCG: state = state * 0x41C64E6D + 0x3039; return
// (state >> 16) & 0x7FFF.
#include "bios_call.h"
#include "game.h"
#include "r3000.h"
#include "stub_game_runtime.h"
#include "testutil.h"

namespace {

void seed(Game &game, uint32_t value) {
  CHECK(psx::test::dispatchBios(game, {'A', 0x30, {value}}));
  CHECK_EQ(game.core.r[psx::cpu::kV0], 0u);
}

uint32_t next(Game &game) {
  return psx::test::biosResult(game, {'A', 0x2F});
}

void test_seed_one_exact_sequence_and_restart() {
  psx::test::StubGameRuntime stubRuntime;
  Game *game = new Game(stubRuntime, {});
  seed(*game, 1);
  constexpr uint32_t expected[] = {16838u, 5758u, 10113u, 17515u, 31051u};
  for (uint32_t value : expected) {
    CHECK_EQ(next(*game), value);
  }

  seed(*game, 1);
  CHECK_EQ(next(*game), expected[0]);
  delete game;
}

void test_state_is_per_game() {
  psx::test::StubGameRuntime stubRuntime;
  Game *a = new Game(stubRuntime, {});
  Game *b = new Game(stubRuntime, {});
  seed(*a, 1);
  seed(*b, 7);
  CHECK_EQ(next(*a), 16838u);
  CHECK_EQ(next(*b), 19564u);
  CHECK_EQ(next(*a), 5758u);
  delete b;
  delete a;
}

void test_wrong_table_and_neighbor_are_not_claimed() {
  psx::test::StubGameRuntime stubRuntime;
  Game *game = new Game(stubRuntime, {});
  game->core.r[psx::cpu::kV0] = 0xDEADBEEFu;
  CHECK(!game->hle.dispatchBios('Z', 0x2F));
  CHECK_EQ(game->core.r[psx::cpu::kV0], 0xDEADBEEFu);
  CHECK(!game->hle.dispatchBios('A', 0x31));
  CHECK_EQ(game->core.r[psx::cpu::kV0], 0xDEADBEEFu);
  delete game;
}

} // namespace

int main() {
  RUN(seed_one_exact_sequence_and_restart);
  RUN(state_is_per_game);
  RUN(wrong_table_and_neighbor_are_not_claimed);
  return psx::test::summary();
}
