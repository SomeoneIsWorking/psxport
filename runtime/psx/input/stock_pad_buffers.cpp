#include "stock_pad_buffers.h"

#include "core.h"

namespace psx::input {

void StockPadBuffers::deliver(Core &core, const PadDelivery &delivery) const {
  const std::uint32_t fixed[2] = {layout_.slot0Buffer, layout_.slot1Buffer};
  const std::uint32_t stride = layout_.slotPointerStride ? layout_.slotPointerStride : 4u;
  for (std::uint32_t slot = 0; slot < 2; ++slot) {
    // Address 0 is not a table: a title without one never reads guest 0 as a buffer pointer.
    std::uint32_t buffer = layout_.slotPointerTable ? core.mem_r32(layout_.slotPointerTable + slot * stride) : 0u;
    if (!buffer) {
      buffer = fixed[slot];
    }
    if (!buffer) {
      continue;
    }
    if (slot == 1 && !delivery.slot1Connected) {
      core.mem_w8(buffer, 0xFF);
      continue;
    }
    for (std::uint32_t index = 0; index < delivery.packet.size(); ++index) {
      core.mem_w8(buffer + index, delivery.packet[index]);
    }
  }
}

} // namespace psx::input
