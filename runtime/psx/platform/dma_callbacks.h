// dma_callbacks.h — per-Game native ownership of Sony DMACallback registrations.
//
// A title-owned native DMACallback body exchanges entries here; a title whose stock libapi keeps the
// table in guest RAM binds psx::hle::StockDmaCallbacks, which then answers instead. Callback identity
// only: the title body still performs the measured DICR read/modify/write that arms its channel.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::hle {
class StockDmaCallbacks;
}

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

  // The title's guest-RAM table, owned by its session; null keeps the native registry.
  void bindGuestTable(const psx::hle::StockDmaCallbacks &table) {
    guestTable_ = &table;
  }
  const psx::hle::StockDmaCallbacks *guestTable() const {
    return guestTable_;
  }

private:
  static constexpr std::size_t index(DmaChannel channel) {
    return static_cast<std::size_t>(channel);
  }

  std::array<uint32_t, 7> callbacks_{};
  const psx::hle::StockDmaCallbacks *guestTable_ = nullptr;
};
