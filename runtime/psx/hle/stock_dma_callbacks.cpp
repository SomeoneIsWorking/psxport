#include "stock_dma_callbacks.h"

#include "dma_irq.h"
#include "game.h"
#include "irq_edge.h"

namespace psx::hle {

void StockDmaCallbacks::bind(Game &game) const & {
  game.dmaCallbacks.bindGuestTable(*this);
}

std::uint32_t StockDmaCallbacks::callback(Core &core, int channel) const {
  const std::uint32_t slot = dma_callback_slot(table_, channel);
  return slot ? core.mem_r32(slot) : 0u;
}

bool StockDmaCallbacks::handlerRuns(const Core &core) {
  return (core.game->hle.i_mask & (1u << IRQ_BIT_DMA)) != 0u;
}

} // namespace psx::hle
