// The fallback block budget bounds interpretation of unsupported blocks, but never a cross-block
// load-delay hazard: that case is a correctness repair (the hazard is a property of the guest's
// branch/load layout, not of how much code was interpreted), so it must not be refused by a limit
// meant for the other reasons.
#include "config.h"
#include "execution_control.h"
#include "execution_exit.h"
#include "game.h"
#include "lightrec_executor.h"

#include "dynarec_test_fixture.h"

#include "testutil.h"

using namespace dynarec_test;

static void test_cross_block_load_delay_hazards_are_not_limited_by_the_fallback_block_budget() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  constexpr std::uint32_t data = 0x00020000u;
  constexpr std::uint32_t iterations = 8u; // more hazard events than the default block limit of one
  core.mem_w32(data, 5u);
  core.mem_w32(entry, 0x24100001u);                   // addiu s0, zero, 1        branch condition, always taken
  core.mem_w32(entry + 4u, 0x3c110002u);              // lui   s1, 0x0002         s1 = data
  core.mem_w32(entry + 8u, 0x24120000u | iterations); // addiu s2, zero, iterations
  core.mem_w32(entry + 12u, 0x24090001u);             // addiu t1, zero, 1        the OLD value of t1
  core.mem_w32(entry + 16u, 0x00005021u);             // addu  t2, zero, zero     accumulator
  core.mem_w32(entry + 20u, 0x1600'0002u);            // loop: bne s0, zero, +2 words -> target
  core.mem_w32(entry + 24u, 0x8e290000u);             // lw    t1, 0(s1)          delay slot: loads 5 into t1
  core.mem_w32(entry + 28u, 0u);                      // not reached
  core.mem_w32(entry + 32u, 0x01495021u);             // target: addu t2, t2, t1  first op reads t1 in the load
                                                      // delay slot, so it must see the OLD t1 = 1
  core.mem_w32(entry + 36u, 0x2652ffffu);             // addiu s2, s2, -1
  core.mem_w32(entry + 40u, 0x1640fffau);             // bne   s2, zero, loop
  core.mem_w32(entry + 44u, 0x24090001u);             // addiu t1, zero, 1        delay slot: restore the old t1
  core.mem_w32(entry + 48u, 0x1000ffffu);             // beq zero, zero, self
  core.mem_w32(entry + 52u, 0u);

  auto &executor = core.lightrecExecutor();
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(2000));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::BudgetExhausted);
  CHECK_EQ(core.r[10], iterations); // each iteration added the OLD t1 (1), never the loaded 5
  CHECK_EQ(core.r[9], 1u);
  CHECK_EQ(executor.counters().fallback.loadDelayHazard, static_cast<std::uint64_t>(iterations));
  CHECK_EQ(executor.counters().fallback.refusedCalls, 0u);
  CHECK(executor.counters().fallback.instructions >= iterations);
  CHECK(executor.counters().fallback.instructions <= 3u * iterations);
  CHECK(executor.counters().executedInstructions > executor.counters().fallback.instructions);
}

static void test_non_hazard_fallbacks_stay_bounded_when_hazards_are_exempt() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  constexpr std::uint32_t entry = 0x00010000u;
  core.mem_w32(entry, 0x10000001u); // beq with a branch in its delay slot (unsupported block)
  core.mem_w32(entry + 4u, 0x08004003u);
  core.mem_w32(entry + 8u, 0u);
  core.mem_w32(entry + 12u, 0x1000ffffu);
  core.mem_w32(entry + 16u, 0u);

  CHECK(psx::config::set_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT", "0"));
  auto &executor = core.lightrecExecutor();
  const auto result = executor.execute(entry, psx::cpu::ExecutionBudget::fromCycles(20));
  CHECK(psx::config::clear_runtime("PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT"));

  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_EQ(executor.counters().fallback.refusedUnsupportedBlock, 1u);
  CHECK_EQ(executor.counters().fallback.loadDelayHazard, 0u);
}

int main() {
  RUN(cross_block_load_delay_hazards_are_not_limited_by_the_fallback_block_budget);
  RUN(non_hazard_fallbacks_stay_bounded_when_hazards_are_exempt);
  return pt_summary();
}
