// stock_pad_buffers.h — a pad driver that polls the packet from fixed guest buffers.
#pragma once

#include "pad_delivery.h"

#include <cstdint>

class Core;

namespace psx::input {

// Writes each slot's packet to the buffer the title's driver reads. A non-null pointer in the
// driver's slot table wins over the fixed buffer; a disconnected slot 1 gets 0xFF.
class StockPadBuffers {
public:
  struct Layout {
    std::uint32_t slot0Buffer = 0;
    std::uint32_t slot1Buffer = 0;
    std::uint32_t slotPointerTable = 0;
    std::uint32_t slotPointerStride = 4;
  };

  explicit StockPadBuffers(const Layout &layout) : layout_(layout) {}
  void deliver(Core &core, const PadDelivery &delivery) const;

private:
  Layout layout_;
};

} // namespace psx::input
