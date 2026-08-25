// The oracle interpreter and static emitter must advance the same deterministic guest-instruction
// clock. One instruction currently contributes one tick; this is ordering time, not R3000 latency.
// tools/recomp/test_emit.py runs this exact four-instruction shape through emitted C and asserts four
// ticks; this side executes the real interpreter loop over guest RAM and asserts the same answer.
#include "testutil.h"

#include <cstdint>

#include "cd_drive_timing.h"
#include "game.h"

void interp_run(Core *core, uint32_t pc);

namespace {

constexpr uint32_t kBase = 0x80010000u;

uint32_t addiu(unsigned rt, unsigned rs, uint16_t immediate) {
  return (9u << 26u) | (rs << 21u) | (rt << 16u) | immediate;
}

uint32_t jr(unsigned rs) {
  return (rs << 21u) | 8u;
}

void test_interpreter_matches_emitted_four_instruction_window(void) {
  auto *game = new Game();
  game->core.mem_w32(kBase + 0, addiu(2, 0, 1)); // addiu v0, zero, 1
  game->core.mem_w32(kBase + 4, addiu(3, 0, 2)); // addiu v1, zero, 2
  game->core.mem_w32(kBase + 8, jr(31));         // jr ra
  game->core.mem_w32(kBase + 12, 0);             // delay-slot nop

  interp_run(&game->core, kBase);

  CHECK_EQ(game->core.r[2], 1);
  CHECK_EQ(game->core.r[3], 2);
  CHECK_EQ(game->timing.guestInstructionTicks, 4);
}

void test_interpreter_ticks_service_the_shipping_cdc_deadline(void) {
  auto *game = new Game();
  game->core.mem_w32(kBase + 0, addiu(2, 0, 1));
  game->core.mem_w32(kBase + 4, addiu(3, 0, 2));
  game->core.mem_w32(kBase + 8, jr(31));
  game->core.mem_w32(kBase + 12, 0);
  game->cdc.reading = 1;
  game->cdc.drive_event_armed = 1;
  // One full sector period out: far enough that executing four instructions (microseconds) cannot
  // let the WALL-LOCKED drive clock cross it. A tiny literal like 4 would already be in the past
  // before the run began.
  game->cdc.drive_deadline_ticks = cd_drive_sector_period_cpu_ticks(0xA0);

  interp_run(&game->core, kBase);

  CHECK_EQ(game->timing.guestInstructionTicks, 4);
  // The CDC drive clock is WALL-LOCKED (see test_cdc_emulated_time.cpp): instruction burn alone
  // must not fire a sector deadline, however much guest work the interpreter executes.
  CHECK_EQ(game->cdc.drive_event_armed, 1);
  CHECK_EQ(game->cdc.following_sector_ready, 0);
}

} // namespace

int main() {
  RUN(interpreter_matches_emitted_four_instruction_window);
  RUN(interpreter_ticks_service_the_shipping_cdc_deadline);
  return pt_summary();
}
