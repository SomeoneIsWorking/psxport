// BIOS A0:0x2F/0x30 rand/srand must be deterministic, per-Game, and reached
// through the same Hle::dispatchBios seam as the guest's tail-jump stubs. The
// sequence is Sony's libc LCG: state = state * 0x41C64E6D + 0x3039; return
// (state >> 16) & 0x7FFF.
#include "../runtime/recomp/game.h"
#include "testutil.h"

namespace {

enum { R_V0 = 2, R_A0 = 4 };

bool bios_a0(Game &game, uint32_t fn, uint32_t arg,
             uint32_t sentinel = 0xDEADBEEFu) {
  game.core.r[R_A0] = arg;
  game.core.r[R_V0] = sentinel;
  return game.hle.dispatchBios('A', fn);
}

void seed(Game &game, uint32_t value) {
  CHECK(bios_a0(game, 0x30, value));
  CHECK_EQ(game.core.r[R_V0], 0u);
}

uint32_t next(Game &game) {
  return bios_a0(game, 0x2F, 0) ? game.core.r[R_V0] : 0xDEADBEEFu;
}

void test_seed_one_exact_sequence_and_restart() {
  Game *game = new Game();
  seed(*game, 1);
  static constexpr uint32_t expected[] = {16838u, 5758u, 10113u, 17515u,
                                          31051u};
  for (uint32_t value : expected)
    CHECK_EQ(next(*game), value);

  seed(*game, 1);
  CHECK_EQ(next(*game), expected[0]);
  delete game;
}

void test_state_is_per_game() {
  Game *a = new Game();
  Game *b = new Game();
  seed(*a, 1);
  seed(*b, 7);
  CHECK_EQ(next(*a), 16838u);
  CHECK_EQ(next(*b), 19564u);
  CHECK_EQ(next(*a), 5758u);
  delete b;
  delete a;
}

void test_wrong_table_and_neighbor_are_not_claimed() {
  Game *game = new Game();
  game->core.r[R_V0] = 0xDEADBEEFu;
  CHECK(!game->hle.dispatchBios('Z', 0x2F));
  CHECK_EQ(game->core.r[R_V0], 0xDEADBEEFu);
  CHECK(!game->hle.dispatchBios('A', 0x31));
  CHECK_EQ(game->core.r[R_V0], 0xDEADBEEFu);
  delete game;
}

} // namespace

int main() {
  RUN(seed_one_exact_sequence_and_restart);
  RUN(state_is_per_game);
  RUN(wrong_table_and_neighbor_are_not_claimed);
  return pt_summary();
}
