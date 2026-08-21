// test_cdc_dma_depletion.cpp — DMA3 reads the CDC's zero value after its data FIFO is depleted.
//
// Spider-Man's stock libstr intentionally starts a fixed 504-word transfer while only the 70-word
// raw-sector tail remains. Beetle's shipping oracle path keeps channel 3 runnable and
// PS_CDC_DMARead returns zero after the FIFO empties, so all 504 programmed words reach RAM: 70 tail
// words followed by 434 zero words. The old framework wrote only 70 and left poison/stale RAM in
// the other 434 slots. These tests drive the shipping cdc_dma_read and Core DMA3 CHCR paths and
// reject both that stale-destination answer and a counterfeit answer sourced from the next sector.
#include "testutil.h"

#include <cstdint>

#include "game.h"

namespace {

constexpr int kRequestedWords = 504;
constexpr int kTailWords = 70;
constexpr uint32_t kDestination = 0x80020000u;
constexpr uint32_t kStale = 0x0E0E0E0Eu;

uint32_t tail_word(int index) {
  return 0xA1000000u | static_cast<uint32_t>(index);
}

void seed_tail(CdcState *cdc) {
  cdc->bfrd = 1;
  cdc->data_n = kTailWords * static_cast<int>(sizeof(uint32_t));
  cdc->data_rd = 0;
  cdc->following_sector_ready = 1;
  for (int index = 0; index < kTailWords; ++index) {
    const uint32_t word = tail_word(index);
    const int offset = index * static_cast<int>(sizeof(uint32_t));
    cdc->data[offset + 0] = static_cast<uint8_t>(word);
    cdc->data[offset + 1] = static_cast<uint8_t>(word >> 8u);
    cdc->data[offset + 2] = static_cast<uint8_t>(word >> 16u);
    cdc->data[offset + 3] = static_cast<uint8_t>(word >> 24u);
  }
}

void test_controller_read_overwrites_stale_output_with_zeros(void) {
  CdcState cdc{};
  seed_tail(&cdc);
  uint32_t output[kRequestedWords];
  for (uint32_t &word : output) {
    word = kStale;
  }

  CHECK_EQ(cdc_dma_read(&cdc, output, kRequestedWords), kTailWords);
  for (int index = 0; index < kTailWords; ++index) {
    CHECK_EQ(output[index], tail_word(index));
  }
  for (int index = kTailWords; index < kRequestedWords; ++index) {
    CHECK_EQ(output[index], 0u); // not stale RAM and not data fabricated from a following sector
  }
  CHECK_EQ(cdc.following_sector_ready, 1); // DMA depletion did not consume/forge the next sector
}

void test_dma3_shipping_path_commits_the_full_programmed_transfer(void) {
  auto *game = new Game();
  seed_tail(&game->cdc);
  for (int index = 0; index < kRequestedWords; ++index) {
    game->core.mem_w32(kDestination + static_cast<uint32_t>(index * 4), kStale);
  }

  game->core.mem_w32(0x1F8010B0u, kDestination);
  game->core.mem_w32(0x1F8010B4u, kRequestedWords);
  game->core.mem_w32(0x1F8010B8u, 0x11000000u);

  for (int index = 0; index < kTailWords; ++index) {
    CHECK_EQ(game->core.mem_r32(kDestination + static_cast<uint32_t>(index * 4)), tail_word(index));
  }
  for (int index = kTailWords; index < kRequestedWords; ++index) {
    CHECK_EQ(game->core.mem_r32(kDestination + static_cast<uint32_t>(index * 4)), 0u);
  }
  CHECK_EQ(game->core.mem_r32(0x1F8010B8u) & 0x01000000u, 0u); // programmed transfer completed
  CHECK_EQ(game->cdc.following_sector_ready, 1);
}

void test_deasserted_bfrd_dma_commits_only_controller_zeros(void) {
  auto *game = new Game();
  seed_tail(&game->cdc);
  game->cdc.bfrd = 0; // FIFO bytes exist internally but are not exposed to CPU/DMA reads.
  for (int index = 0; index < kRequestedWords; ++index) {
    game->core.mem_w32(kDestination + static_cast<uint32_t>(index * 4), kStale);
  }

  game->core.mem_w32(0x1F8010B0u, kDestination);
  game->core.mem_w32(0x1F8010B4u, kRequestedWords);
  game->core.mem_w32(0x1F8010B8u, 0x11000000u);

  for (int index = 0; index < kRequestedWords; ++index) {
    CHECK_EQ(game->core.mem_r32(kDestination + static_cast<uint32_t>(index * 4)), 0u);
  }
  CHECK_EQ(game->cdc.data_rd, 0); // deasserted BFRD did not expose the internal tail
  CHECK_EQ(game->core.mem_r32(0x1F8010B8u) & 0x01000000u, 0u);
}

} // namespace

int main() {
  RUN(controller_read_overwrites_stale_output_with_zeros);
  RUN(dma3_shipping_path_commits_the_full_programmed_transfer);
  RUN(deasserted_bfrd_dma_commits_only_controller_zeros);
  return pt_summary();
}
