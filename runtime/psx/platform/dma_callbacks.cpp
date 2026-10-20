#include "dma_callbacks.h"

#include <utility>

uint32_t DmaCallbackRegistry::exchange(psx::dma::DmaChannel channel, uint32_t callback) {
  return std::exchange(callbacks_[index(channel)], callback);
}

uint32_t DmaCallbackRegistry::current(psx::dma::DmaChannel channel) const {
  return callbacks_[index(channel)];
}
