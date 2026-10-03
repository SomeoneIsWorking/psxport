#include "dma_callbacks.h"

#include <utility>

uint32_t DmaCallbackRegistry::exchange(DmaChannel channel, uint32_t callback) {
  return std::exchange(callbacks_[index(channel)], callback);
}

uint32_t DmaCallbackRegistry::current(DmaChannel channel) const {
  return callbacks_[index(channel)];
}
