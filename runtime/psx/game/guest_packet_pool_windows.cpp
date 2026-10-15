#include "guest_packet_pool_windows.h"

#include "core.h"

namespace psx::gpu {
namespace {

constexpr std::uint32_t kMainRamEnd = 0x80200000u;

std::uint32_t mainRamAddress(std::uint32_t value) {
  return (value & 0x1FFFFFFFu) | 0x80000000u;
}

void add(PacketPoolExtent &extent, std::uint32_t low, std::uint32_t high) {
  if (low < high && high <= kMainRamEnd) {
    extent.low[extent.count] = low;
    extent.high[extent.count] = high;
    ++extent.count;
  }
}

} // namespace

bool PacketPoolExtent::contains(std::uint32_t address) const {
  for (std::uint32_t pool = 0; pool < count; ++pool) {
    if (address >= low[pool] && address < high[pool]) {
      return true;
    }
  }
  return false;
}

bool PacketPoolExtent::readFrom(GuestSpan store) const {
  const std::uint32_t storeLow = mainRamAddress(store.address);
  const std::uint32_t storeHigh = storeLow + store.bytes;
  for (const std::uint32_t source : sources) {
    if (source && storeLow < mainRamAddress(source) + 4u && mainRamAddress(source) < storeHigh) {
      return true;
    }
  }
  return false;
}

PacketPoolExtent GuestPacketPoolWindows::extent(Core &core) const {
  PacketPoolExtent result;
  switch (representation) {
  case Representation::FixedBaseStride:
    add(result, mainRamAddress(base), mainRamAddress(base) + 2u * stride);
    break;
  case Representation::SingleWindow:
    add(result, mainRamAddress(base), mainRamAddress(end));
    break;
  case Representation::LiveBaseEndPointers:
    for (std::size_t pool = 0; pool < basePointer.size(); ++pool) {
      if (!basePointer[pool] || !endPointer[pool]) {
        continue;
      }
      result.sources[2 * pool] = basePointer[pool];
      result.sources[(2 * pool) + 1] = endPointer[pool];
      // A heap pool may not be allocated yet during boot; a zero bound is no pool.
      const std::uint32_t low = core.mem_r32(basePointer[pool]);
      const std::uint32_t high = core.mem_r32(endPointer[pool]);
      if (low && high) {
        add(result, mainRamAddress(low), mainRamAddress(high));
      }
    }
    break;
  }
  return result;
}

} // namespace psx::gpu
