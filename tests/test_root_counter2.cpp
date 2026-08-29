// Root counter 2 (0x1F801120 value / 0x1F801124 mode / 0x1F801128 target) — Timing::rootCounter2*.
//
// Guest code uses this register as a stopwatch: latch the value, spin until the delta exceeds a
// budget. While it was unmapped I/O reading 0, every such delta was 0, every budget was unreachable,
// and the spin never ended. These cases assert the two properties that makes it usable — it advances
// with emulated CPU time, and it wraps at 16 bits — plus the mode/target plumbing around it.
#include "../runtime/recomp/game.h"
#include "testutil.h"

namespace {

constexpr uint32_t kCounter = 0x1F801120u;
constexpr uint32_t kMode = 0x1F801124u;
constexpr uint32_t kTarget = 0x1F801128u;

// Advance emulated time the way the running guest does, through the instruction-tick path.
void burn(Game &game, uint64_t ticks) {
  while (ticks) {
    const uint32_t chunk = ticks > 0xFFFFu ? 0xFFFFu : (uint32_t)ticks;
    game.timing.advanceGuestInstructionTicks(chunk);
    ticks -= chunk;
  }
}

void test_counter_advances_with_emulated_cpu_time() {
  Game *game = new Game();
  game->core.mem_w16(kMode, 0); // mode 0: system clock, and resets the counter
  CHECK_EQ(game->core.mem_r16(kCounter), 0u);
  burn(*game, 1000);
  CHECK_EQ(game->core.mem_r16(kCounter), 1000u);
  burn(*game, 500);
  CHECK_EQ(game->core.mem_r16(kCounter), 1500u);
  delete game;
}

// Mode bit 9 selects system-clock/8, and the guest's own timeout helper normalises against it.
void test_mode_bit9_selects_the_divided_clock() {
  Game *game = new Game();
  game->core.mem_w16(kMode, 0x200u);
  burn(*game, 800);
  CHECK_EQ(game->core.mem_r16(kCounter), 100u);
  delete game;
}

void test_sync_mode_matrix_stops_only_modes_zero_and_three() {
  const uint16_t modes[] = {0x001u, 0x003u, 0x005u, 0x007u};
  const uint16_t expected[] = {0x1234u, 0x12D4u, 0x12D4u, 0x1234u};
  for (unsigned i = 0; i < 4; ++i) {
    Game *game = new Game();
    game->core.mem_w16(kMode, modes[i]);
    game->core.mem_w16(kCounter, 0x1234u);
    burn(*game, 0xA0u);
    CHECK_EQ(game->core.mem_r16(kCounter), expected[i]);
    delete game;
  }
  CHECK_EQ(sizeof(modes) / sizeof(modes[0]), 4u);
}

// It is a 16-bit register. A guest that latches near the top and spins has to see it come round —
// the measured helper (guest 0x8003C6A8) has an explicit wrap correction that only makes sense if
// the value wraps.
void test_counter_wraps_at_16_bits() {
  Game *game = new Game();
  game->core.mem_w16(kMode, 0);
  burn(*game, 0x10000u - 4u);
  CHECK_EQ(game->core.mem_r16(kCounter), 0xFFFCu);
  burn(*game, 8);
  CHECK_EQ(game->core.mem_r16(kCounter), 4u);
  delete game;
}

// Writing the counter restarts it from that value; writing the mode zeroes it.
void test_writes_reposition_and_reset() {
  Game *game = new Game();
  game->core.mem_w16(kMode, 0);
  burn(*game, 5000);
  game->core.mem_w16(kCounter, 0x1234u);
  CHECK_EQ(game->core.mem_r16(kCounter), 0x1234u);
  burn(*game, 0x10);
  CHECK_EQ(game->core.mem_r16(kCounter), 0x1244u);
  game->core.mem_w16(kMode, 0x200u);
  CHECK_EQ(game->core.mem_r16(kCounter), 0u);
  delete game;
}

void test_target_is_stored_and_readable() {
  Game *game = new Game();
  game->core.mem_w16(kTarget, 0xABCDu);
  CHECK_EQ(game->core.mem_r16(kTarget), 0xABCDu);
  delete game;
}

void test_target_reset_wraps_at_the_programmed_period() {
  Game *game = new Game();
  game->core.mem_w16(kTarget, 100u);
  game->core.mem_w16(kMode, 0x008u); // system clock, reset on target
  burn(*game, 99u);
  CHECK_EQ(game->core.mem_r16(kCounter), 99u);
  burn(*game, 1u);
  CHECK_EQ(game->core.mem_r16(kCounter), 0u);
  burn(*game, 7u);
  CHECK_EQ(game->core.mem_r16(kCounter), 7u);
  delete game;

  Game *divided = new Game();
  divided->core.mem_w16(kTarget, 100u);
  divided->core.mem_w16(kMode, 0x208u); // clock/8, reset on target
  burn(*divided, 800u);
  CHECK_EQ(divided->core.mem_r16(kCounter), 0u);
  burn(*divided, 40u);
  CHECK_EQ(divided->core.mem_r16(kCounter), 5u);
  delete divided;
}

// A stopwatch measured through the SAME code path the guest uses: latch, spin, compare. This is the
// shape that could not terminate before, so it is asserted end to end rather than by inspection.
void test_a_latch_and_spin_budget_terminates() {
  Game *game = new Game();
  game->core.mem_w16(kMode, 0x200u); // the driver's units: system clock / 8
  const uint32_t start = game->core.mem_r16(kCounter);
  const uint32_t budget = 0x1AEu; // the measured pad-driver ack timeout
  int spins = 0;
  while ((uint32_t)((game->core.mem_r16(kCounter) - start) & 0xFFFFu) < budget) {
    burn(*game, 64);
    if (++spins > 10000) {
      break; // the pre-fix behaviour: assert on the count so a non-terminating spin cannot pass
    }
  }
  CHECK(spins > 0);
  CHECK(spins < 10000);
  delete game;
}

} // namespace

int main() {
  RUN(counter_advances_with_emulated_cpu_time);
  RUN(mode_bit9_selects_the_divided_clock);
  RUN(sync_mode_matrix_stops_only_modes_zero_and_three);
  RUN(counter_wraps_at_16_bits);
  RUN(writes_reposition_and_reset);
  RUN(target_is_stored_and_readable);
  RUN(target_reset_wraps_at_the_programmed_period);
  RUN(a_latch_and_spin_budget_terminates);
  return pt_summary();
}
