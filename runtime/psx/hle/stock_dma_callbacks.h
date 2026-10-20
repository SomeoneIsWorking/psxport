// stock_dma_callbacks.h — libapi DMACallback keeping its per-channel table in guest RAM.
#pragma once

#include "dma_irq.h"

#include <cstdint>

class Core;
class Game;

namespace psx::hle {

// Each completed transfer goes to the current word at `table + 4 * channel`, as the BIOS DMA handler
// reads it. A title that binds none keeps its callbacks in the native DmaCallbackRegistry.
class StockDmaCallbacks {
public:
  explicit StockDmaCallbacks(std::uint32_t table) : table_(table) {}

  void bind(Game &game) const &;
  // The Game keeps a pointer to it, so a temporary cannot bind.
  void bind(Game &game) const && = delete;

  // The callback the table holds for `channel` now; 0 when none.
  std::uint32_t callback(Core &core, psx::dma::DmaChannel channel) const;
  // libapi masks the DMA line around its GPU queue loops, and its handler runs only while it is open.
  static bool handlerRuns(const Core &core);

private:
  std::uint32_t table_;
};

} // namespace psx::hle
