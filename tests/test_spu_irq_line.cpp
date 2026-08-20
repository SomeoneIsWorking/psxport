// test_spu_irq_line.cpp — the SPU IRQ line reaches I_STAT, and does so EDGE-latched.
//
// spu_beetle.c's IRQ_Assert used to be an unconditional no-op marked STOPGAP: the vendored Beetle SPU
// computed the SPU IRQ correctly (SPUCNT bit 6 + IRQAddr matched against SPU-RAM addresses) and the
// runtime simply threw the line away, so a guest that services the SPU interrupt never ran its
// handler. The delivery rule now lives in runtime/recomp/irq_edge.h and hw_bind.cpp routes the line
// through it into Game::hle.i_stat bit 9.
//
// The no-op stopgap fails every case below that asserts a bit ever appears; the LEVEL-driven mistake
// (the obvious wrong "fix") fails the ack cases.
#include "../runtime/recomp/irq_edge.h"
#include "testutil.h"

// A source going low->high sets its bit. That is the whole point, and it is what the no-op dropped.
static void test_rising_edge_sets_the_bit(void) {
  uint32_t level = 0;
  uint32_t i_stat = irq_latch_edge(level, 0, IRQ_BIT_SPU, true);
  CHECK_EQ(i_stat, 1u << 9);
  CHECK_EQ(level, 1u << 9);
  CHECK_EQ(IRQ_BIT_SPU, 9); // I_STAT bit 9 is the SPU (beetle-psx irq.h IRQ_SPU)
}

// Holding the line high does not keep re-setting it: after the guest acks I_STAT, a still-asserted
// source must NOT immediately re-raise. A level-driven implementation fails exactly here.
static void test_held_line_does_not_re_raise_after_ack(void) {
  uint32_t level = 0;
  uint32_t i_stat = irq_latch_edge(level, 0, IRQ_BIT_SPU, true);
  CHECK_EQ(i_stat, 1u << 9);
  i_stat &= ~(1u << 9);                                      // the guest acks (mem.cpp I_STAT write)
  i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, true); // source still asserted
  CHECK_EQ(i_stat, 0u);
  CHECK_EQ(level, 1u << 9); // …but the LEVEL is still high
}

// Deasserting must not clear a pending, un-acked I_STAT bit — the CPU has not seen it yet.
static void test_falling_edge_does_not_clear_pending(void) {
  uint32_t level = 0;
  uint32_t i_stat = irq_latch_edge(level, 0, IRQ_BIT_SPU, true);
  i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, false); // SPUCNT bit 6 cleared
  CHECK_EQ(i_stat, 1u << 9);
  CHECK_EQ(level, 0u);
}

// Low -> high -> low -> high after an ack raises again. Without this, one SPU-RAM address match would
// be the only interrupt a streaming voice ever got.
static void test_re_arm_after_ack_raises_again(void) {
  uint32_t level = 0, i_stat = 0;
  int raises = 0;
  for (int i = 0; i < 5; i++) {
    i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, true);
    if (i_stat & (1u << 9)) {
      raises++;
    }
    i_stat &= ~(1u << 9);                                       // guest acks
    i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, false); // source deasserts
  }
  CHECK_EQ(raises, 5); // denominator: 5 assert/ack/deassert cycles, all 5 delivered
  CHECK_EQ(i_stat, 0u);
}

// The SPU line must not disturb any other source's bit — bit 2 (CD) is live in this framework today.
static void test_other_sources_are_untouched(void) {
  uint32_t level = 1u << IRQ_BIT_CD; // CD already asserted
  uint32_t i_stat = 1u << IRQ_BIT_CD;
  i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, true);
  CHECK_EQ(i_stat, (1u << IRQ_BIT_CD) | (1u << IRQ_BIT_SPU));
  i_stat = irq_latch_edge(level, i_stat, IRQ_BIT_SPU, false);
  CHECK_EQ(level, 1u << IRQ_BIT_CD); // CD's LEVEL survived the SPU deassert
  CHECK_EQ(i_stat, (1u << IRQ_BIT_CD) | (1u << IRQ_BIT_SPU));
}

// The rule is generic, so run it over every source bit this framework names and assert the count.
static void test_every_named_source_bit_latches(void) {
  const int bits[] = {IRQ_BIT_VBLANK,
                      IRQ_BIT_GPU,
                      IRQ_BIT_CD,
                      IRQ_BIT_DMA,
                      IRQ_BIT_TIMER0,
                      IRQ_BIT_TIMER1,
                      IRQ_BIT_TIMER2,
                      IRQ_BIT_SIO,
                      IRQ_BIT_SPU,
                      IRQ_BIT_PIO};
  int scanned = 0;
  for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++) {
    uint32_t level = 0;
    CHECK_EQ(irq_latch_edge(level, 0, bits[i], true), 1u << bits[i]);
    scanned++;
  }
  CHECK_EQ(scanned, 10); // denominator: all 10 named sources, not a sample
}

int main(void) {
  RUN(rising_edge_sets_the_bit);
  RUN(held_line_does_not_re_raise_after_ack);
  RUN(falling_edge_does_not_clear_pending);
  RUN(re_arm_after_ack_raises_again);
  RUN(other_sources_are_untouched);
  RUN(every_named_source_bit_latches);
  return pt_summary();
}
