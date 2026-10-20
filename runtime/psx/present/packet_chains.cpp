#include "packet_chains.h"

#include "guest_memory_map.h"
#include "ordering_table.h"

#include <lucent/log.h>

#include <array>
#include <cstdlib>
#include <span>

namespace psx::present {
namespace {

// A tag's 8-bit count bounds a packet.
constexpr std::size_t kMaxPacketWords = 255;

} // namespace

void PacketChainWalker::emitChain(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink) {
  std::array<std::uint32_t, kMaxPacketWords> words{};
  for (gpu::OtNode tag(0, gpu::OtHeaderWord{link}); !tag.endsChain();) {
    const std::uint32_t packet = gpu::guestAddressOf(tag.nextRamOffset());
    tag = gpu::OtNode(gpu::mainRamOffsetOf(packet), gpu::OtHeaderWord{memory_.mem_r32(packet)});
    const std::uint32_t count = tag.gp0WordCount();
    const std::uint32_t arenaOffset = packet - kseg0Address(bound_.arena.address);
    const bool outside = bound_.arena.bytes != 0 &&
                         (arenaOffset >= bound_.arena.bytes || arenaOffset + 4u * (count + 1u) > bound_.arena.bytes);
    if (++walked_ > bound_.packetLimit || outside) {
      lucent::error("packet-chain",
                    "a {}-word packet at 0x{:08X} past {} packets or outside [0x{:08X},+0x{:X})",
                    count,
                    packet,
                    bound_.packetLimit,
                    bound_.arena.address,
                    bound_.arena.bytes);
      std::abort();
    }
    if (!tag.carriesPrimitive()) {
      continue;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      words[i] = memory_.mem_r32(packet + 4u * (i + 1u));
    }
    const std::span<const std::uint32_t> command(words.data(), count);
    if (drawMode.take(command)) {
      continue;
    }
    std::optional<DrawPrimitive> primitive = gpu::decodePacketPrimitive(command);
    if (!primitive) {
      lucent::error("packet-chain", "the packet 0x{:08X} at 0x{:08X} is not a polygon, line or sprite", words[0], packet);
      std::abort();
    }
    drawMode.apply(*primitive);
    sink.emit(slot, *primitive);
  }
}

void PacketChainWalker::emitBuckets(const BucketHeads &heads, DrawModeTracker &drawMode, PrimitiveSink &sink) {
  for (std::uint32_t bucket = heads.buckets; bucket-- > 0;) {
    emitChain(memory_.mem_r32(heads.base + bucket * heads.stride + heads.headOffset),
              OtSlot{heads.first.table, heads.first.index + bucket},
              drawMode,
              sink);
  }
}

} // namespace psx::present
