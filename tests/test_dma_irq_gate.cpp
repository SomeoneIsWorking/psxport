// test_dma_irq_gate.cpp — DICR decides WHICH DMA completions the guest hears about.
//
// The rule under test is `runtime/recomp/dma_irq.h`. Before it existed, DPCR and DICR were not
// modelled at all — reads returned 0, writes fell through to the stray-I/O path — so the runtime
// signalled the guest's DMA-completion callback on EVERY transfer. That is the bug this file pins:
// a port that cannot see the guest's per-channel interrupt enable cannot tell a transfer the guest
// wanted announced from one it deliberately left silent.
//
// The sequences below are not invented. They are transcriptions of Spider-Man's own code, read out
// of the executable with Ghidra (spider1 tools/ghidra_query.py):
//
//   * libcd DMACallback, 0x8009152C — `DICR = (DICR & 0x00FFFFFF) | (1 << (16+ch)) | 0x00800000`,
//     i.e. registering a callback arms that channel AND the master enable.
//   * libstr's DMA starter, FUN_80085948 — read-modify-writes the BYTE at DICR+2 per transfer:
//     `*(u8*)(DICR+2) |= 1<<ch` when this transfer should raise, `&= ~(1<<ch)` when it should not.
//     The pointer global it goes through, 0x800B0FD4, holds 0x1F8010F4 (verified by dumping it).
//   * libstr's sector-arrival handler, FUN_80085000 — enables the interrupt for the LAST chunk of
//     an STR frame only, so one frame of N sectors owes the guest exactly ONE completion.
//
// Hermetic: pure integer rules, no Core, no disc, no GPU.
#include "../runtime/recomp/dma_irq.h"
#include "testutil.h"

// Channel 3 is the CD-ROM's. Named rather than spelled 3 at fifteen call sites.
static constexpr int CH_CD = 3;
static constexpr uint32_t DICR_ADDR = 0x1F8010F4u;

// libcd DMACallback(ch, fn), 0x8009152C, as a store of the value it computes.
static uint32_t dmacallback_arm(uint32_t dicr, int ch) {
  const uint32_t v = (dicr & 0x00FFFFFFu) | (1u << (16 + ch)) | 0x00800000u;
  return dma_dicr_store(dicr, DICR_ADDR, v, 4);
}

// libstr FUN_80085948's per-transfer byte read-modify-write of DICR+2.
static uint32_t str_set_transfer_irq(uint32_t dicr, int ch, bool raise) {
  const uint32_t byte2 = (dma_dicr_read(dicr) >> 16) & 0xFFu;
  const uint32_t nv = raise ? (byte2 | (1u << ch)) : (byte2 & ~(1u << ch));
  return dma_dicr_store(dicr, DICR_ADDR + 2u, nv, 1);
}

// ---------------------------------------------------------------------------------------------
// A guest that has armed nothing is owed nothing. Denominator stated: all seven channels checked,
// not just the CD's — "no channel is armed" is only meaningful if every channel was asked.
static void test_reset_state_arms_no_channel(void) {
  const uint32_t dicr = 0;
  int armed = 0;
  for (int ch = 0; ch < 7; ch++) {
    if (dma_irq_armed(dicr, ch)) {
      armed++;
    }
  }
  CHECK_EQ(armed, 0);
  CHECK_EQ(dma_dicr_read(dicr), 0u);
  // And a completion changes nothing, on every channel.
  for (int ch = 0; ch < 7; ch++) {
    CHECK_EQ(dma_dicr_complete(dicr, ch), dicr);
  }
}

// Registering a DMA callback arms exactly one channel, plus the master enable.
static void test_dmacallback_arms_master_and_one_channel(void) {
  const uint32_t dicr = dmacallback_arm(0, CH_CD);
  CHECK((dicr & DICR_MASTER_EN) != 0);
  CHECK(dma_irq_armed(dicr, CH_CD));
  int others = 0;
  for (int ch = 0; ch < 7; ch++) {
    if (ch != CH_CD && dma_irq_armed(dicr, ch)) {
      others++;
    }
  }
  CHECK_EQ(others, 0);
}

// The per-transfer byte write is the discriminator the runtime was blind to. It must be able to
// DISARM a channel the callback registration armed, and must not disturb the master enable that
// lives in the same byte.
static void test_per_transfer_byte_write_toggles_only_that_channel(void) {
  uint32_t dicr = dmacallback_arm(0, CH_CD);
  dicr = str_set_transfer_irq(dicr, CH_CD, false);
  CHECK(!dma_irq_armed(dicr, CH_CD));
  CHECK((dicr & DICR_MASTER_EN) != 0); // same byte — must survive
  dicr = str_set_transfer_irq(dicr, CH_CD, true);
  CHECK(dma_irq_armed(dicr, CH_CD));
  CHECK((dicr & DICR_MASTER_EN) != 0);
}

// A completion sets the channel's flag only when armed, and never touches another channel's.
static void test_completion_sets_the_flag_only_when_armed(void) {
  uint32_t armed = dmacallback_arm(0, CH_CD);
  uint32_t after = dma_dicr_complete(armed, CH_CD);
  CHECK_EQ(after & DICR_FLAG_MASK, 1u << (24 + CH_CD));
  CHECK((dma_dicr_read(after) & DICR_MASTER_FLAG) != 0);

  uint32_t disarmed = str_set_transfer_irq(armed, CH_CD, false);
  uint32_t after2 = dma_dicr_complete(disarmed, CH_CD);
  CHECK_EQ(after2 & DICR_FLAG_MASK, 0u);
  CHECK((dma_dicr_read(after2) & DICR_MASTER_FLAG) == 0);
}

// Flags acknowledge on a 1, and only in the lanes the store actually covers.
static void test_flags_are_write_one_to_clear_in_written_lanes_only(void) {
  uint32_t dicr = dma_dicr_complete(dmacallback_arm(0, CH_CD), CH_CD);
  CHECK_EQ(dicr & DICR_FLAG_MASK, 1u << (24 + CH_CD));
  // A byte write to the ENABLE lane must not clear the flag lane.
  uint32_t keep = str_set_transfer_irq(dicr, CH_CD, true);
  CHECK_EQ(keep & DICR_FLAG_MASK, 1u << (24 + CH_CD));
  // Writing 0 to the flag lane leaves it set; writing 1 clears it.
  uint32_t still = dma_dicr_store(dicr, DICR_ADDR + 3u, 0u, 1);
  CHECK_EQ(still & DICR_FLAG_MASK, 1u << (24 + CH_CD));
  uint32_t acked = dma_dicr_store(dicr, DICR_ADDR + 3u, 1u << CH_CD, 1);
  CHECK_EQ(acked & DICR_FLAG_MASK, 0u);
}

// Bit 31 is read-only and computed. Writing it must not stick; forcing via bit 15 must show.
static void test_master_flag_is_read_only_and_computed(void) {
  uint32_t dicr = dma_dicr_store(0, DICR_ADDR, DICR_MASTER_FLAG, 4);
  CHECK_EQ(dicr & DICR_MASTER_FLAG, 0u);
  CHECK_EQ(dma_dicr_read(dicr) & DICR_MASTER_FLAG, 0u);
  uint32_t forced = dma_dicr_store(0, DICR_ADDR, DICR_FORCE, 4);
  CHECK((dma_dicr_read(forced) & DICR_MASTER_FLAG) != 0);
}

// THE REGRESSION CASE. An STR frame of 10 sectors, driven exactly as FUN_80085000 drives it: the
// 8-word header transfer and the first nine data transfers are started with the channel interrupt
// DISABLED, the tenth (last chunk of the frame) with it ENABLED. The guest is owed exactly ONE
// completion for the frame. Ungated, it is signalled 11 times — which is what marked ring slots
// ready before the frame existed and deadlocked the intro movies.
static void test_str_frame_owes_exactly_one_completion(void) {
  uint32_t dicr = dmacallback_arm(0, CH_CD); // FUN_80086030 -> DMACallback(3, 0x8008DB44)
  const int chunks = 10;
  int transfers = 0, signalled = 0;
  for (int i = 0; i < chunks; i++) {
    const bool last = (i == chunks - 1);
    dicr = str_set_transfer_irq(dicr, CH_CD, false); // header DMA: 8 words, never raises
    transfers++;
    if (dma_irq_armed(dicr, CH_CD)) {
      signalled++;
    }
    dicr = str_set_transfer_irq(dicr, CH_CD, last); // data DMA: 0x1F8 words
    transfers++;
    if (dma_irq_armed(dicr, CH_CD)) {
      signalled++;
      dicr = dma_dicr_complete(dicr, CH_CD);
    }
  }
  CHECK_EQ(transfers, 2 * chunks); // denominator: every transfer was examined
  CHECK_EQ(signalled, 1);
  CHECK_EQ(dicr & DICR_FLAG_MASK, 1u << (24 + CH_CD));
}

// ---------------------------------------------------------------------------------------------
// EVERY ARMED CHANNEL IS OWED ITS CALLBACK, NOT JUST THE CD'S.
//
// Measured on Spider-Man, and the reason these exist: the intro FMV player registers an MDEC-out
// (channel 1) completion callback — `FUN_80085BC0(0x8002B28C)` -> `DMACallback(1, ...)`, and the
// DICR the guest actually writes is 0x009A0000, i.e. master + channels 1, 3 and 4. The runtime
// signalled channel 3 and nothing else, so 0x8002B28C was NEVER CALLED over a whole run while its
// registrar ran 4 times and the movie loop pumped 4653 iterations. That callback is what uploads the
// decoded strip to VRAM, so the movie decoded and never appeared.
static constexpr int CH_MDEC_IN = 0, CH_MDEC_OUT = 1, CH_SPU = 4;

static void test_a_completion_signals_the_channel_that_completed(void) {
  uint32_t dicr = dmacallback_arm(dmacallback_arm(0, CH_MDEC_OUT), CH_CD);
  DmaDone done;
  CHECK(done.complete(dicr, CH_MDEC_OUT));
  CHECK(done.owed(CH_MDEC_OUT));
  CHECK(!done.owed(CH_CD)); // nothing completed on the CD channel
  CHECK_EQ(dicr & DICR_FLAG_MASK, 1u << (24 + CH_MDEC_OUT));
  // Denominator: no OTHER channel was left owed either.
  int owed = 0;
  for (int ch = 0; ch < 7; ch++) {
    if (done.owed(ch)) {
      owed++;
    }
  }
  CHECK_EQ(owed, 1);
}

static void test_an_unarmed_channel_is_owed_nothing(void) {
  uint32_t dicr = dmacallback_arm(0, CH_CD); // only the CD channel armed
  DmaDone done;
  CHECK(!done.complete(dicr, CH_MDEC_OUT));
  CHECK(!done.owed(CH_MDEC_OUT));
  CHECK_EQ(dicr & DICR_FLAG_MASK, 0u);
}

static void test_two_channels_completing_in_one_window_both_survive(void) {
  // The MDEC pump drains DMA0 and DMA1 inside ONE guest store, so both can finish before the
  // runtime reaches its next safe dispatch point. A single pending FLAG drops one of them.
  uint32_t dicr = dmacallback_arm(dmacallback_arm(0, CH_MDEC_IN), CH_MDEC_OUT);
  DmaDone done;
  CHECK(done.complete(dicr, CH_MDEC_IN));
  CHECK(done.complete(dicr, CH_MDEC_OUT));
  CHECK(done.owed(CH_MDEC_IN));
  CHECK(done.owed(CH_MDEC_OUT));
  done.taken(CH_MDEC_IN);
  CHECK(!done.owed(CH_MDEC_IN));
  CHECK(done.owed(CH_MDEC_OUT)); // taking one must not clear the other
}

static void test_callback_slot_is_per_channel(void) {
  // Spider-Man's table base, read out of libcd's DMACallback at 0x8009152C (it computes
  // 0x800B4388 + index*4). Slot 3 is the CD callback the earlier fix used directly.
  const uint32_t TABLE = 0x800B4388u;
  CHECK_EQ(dma_callback_slot(TABLE, CH_CD), 0x800B4394u);
  CHECK_EQ(dma_callback_slot(TABLE, CH_MDEC_OUT), 0x800B438Cu);
  for (int ch = 0; ch < 7; ch++) {
    CHECK_EQ(dma_callback_slot(TABLE, ch), TABLE + 4u * (uint32_t)ch);
  }
  // A game that has not RE'd its table gets no slot at all, on every channel.
  int nonzero = 0;
  for (int ch = 0; ch < 7; ch++) {
    if (dma_callback_slot(0, ch)) {
      nonzero++;
    }
  }
  CHECK_EQ(nonzero, 0);
}

static void test_the_spu_channel_is_owed_when_the_guest_armed_it(void) {
  // Not hypothetical: the DICR Spider-Man writes has bit 20 set. A rule that hard-codes one channel
  // is wrong for this one too, and stating it here keeps the gate from being re-narrowed later.
  uint32_t dicr = dmacallback_arm(0, CH_SPU);
  DmaDone done;
  CHECK(done.complete(dicr, CH_SPU));
  CHECK(done.owed(CH_SPU));
}

int main(void) {
  RUN(reset_state_arms_no_channel);
  RUN(dmacallback_arms_master_and_one_channel);
  RUN(per_transfer_byte_write_toggles_only_that_channel);
  RUN(completion_sets_the_flag_only_when_armed);
  RUN(flags_are_write_one_to_clear_in_written_lanes_only);
  RUN(master_flag_is_read_only_and_computed);
  RUN(str_frame_owes_exactly_one_completion);
  RUN(a_completion_signals_the_channel_that_completed);
  RUN(an_unarmed_channel_is_owed_nothing);
  RUN(two_channels_completing_in_one_window_both_survive);
  RUN(callback_slot_is_per_channel);
  RUN(the_spu_channel_is_owed_when_the_guest_armed_it);
  return pt_summary();
}
