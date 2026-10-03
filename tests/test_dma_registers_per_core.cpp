// test_dma_registers_per_core.cpp — DPCR/DICR and the channel-3 registers are state of ONE machine.
//
// They were file-scope statics in mem.cpp, so a second boot in the same process (a title selector
// starting another title, or the oracle's second core) began with the first one's DICR flags and owed
// completions instead of the power-on state. `DmaRegisters` is the per-Core home; this pins that a fresh
// one is the power-on state and that two instances share nothing.
#include "dma_irq.h"
#include "testutil.h"

static void test_fresh_registers_are_power_on(void) {
  DmaRegisters dma;
  CHECK_EQ(dma.dpcr, DPCR_RESET);
  CHECK_EQ(dma.dicr, 0u);
  CHECK_EQ(dma.dma3Madr, 0u);
  CHECK_EQ(dma.dma3Chcr, 0u);
  CHECK_EQ(dma.done.mask, 0u);
}

static void test_a_second_instance_does_not_inherit_the_first(void) {
  DmaRegisters first;
  first.dicr = dma_dicr_store(first.dicr, 0x1F8010F4u, DICR_MASTER_EN | (1u << (16 + 3)), 4);
  CHECK(first.done.complete(first.dicr, 3));
  CHECK(first.done.owed(3));
  DmaRegisters second;
  CHECK(!second.done.owed(3));
  CHECK_EQ(second.dicr, 0u);
}

int main(void) {
  RUN(fresh_registers_are_power_on);
  RUN(a_second_instance_does_not_inherit_the_first);
  return pt_summary();
}
