// dma_callbacks.h — per-Game native ownership of Sony DMACallback registrations.
//
// Legacy adapter runtimes retain the callback table in guest RAM at
// GameConfig::dmaCallbackTable. Direct runtimes have no GameConfig, so a title-owned native
// DMACallback body exchanges entries here instead. This state deliberately owns callback identity
// only: the title body still performs the measured DICR read/modify/write that arms its channel.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class DmaChannel : uint8_t {
  MdecIn = 0,
  MdecOut = 1,
  Gpu = 2,
  Cdrom = 3,
  Spu = 4,
  Pio = 5,
  Otc = 6,
};

class DmaCallbackRegistry {
public:
  // Sony DMACallback returns the callback previously registered for this channel.
  uint32_t exchange(DmaChannel channel, uint32_t callback);
  uint32_t current(DmaChannel channel) const;

private:
  static constexpr std::size_t index(DmaChannel channel) {
    return static_cast<std::size_t>(channel);
  }

  std::array<uint32_t, 7> callbacks_{};
};
