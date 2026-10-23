#include "packet_chains.h"

#include "guest_memory_map.h"
#include "ordering_table.h"

#include <lucent/log.h>

#include <cstdlib>
#include <span>

namespace psx::present {

std::optional<std::uint32_t>
PacketChainWalker::walk(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink) {
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
    const std::span<std::uint32_t> command(words_.data(), count);
    const std::span<gte::WordPlace> commandPlaces(places_.data(), count);
    memory_.readWords(packet + 4u, command);
    memory_.readPlaces(packet + 4u, commandPlaces);
    const gpu::Gp0Command head(words_[0]);
    if (drawMode.take(command) || head.isDrawEnvironmentCommand() || head.isDiscardedNoOp()) {
      continue;
    }
    if (head.isPolyLine()) {
      const bool terminated = gpu::decodePolyLine({command, commandPlaces}, [&](DrawPrimitive segment) {
        drawMode.apply(segment);
        segment.sourceAddress = packet;
        sink.emit(slot, segment);
      });
      if (!terminated) {
        return packet;
      }
      continue;
    }
    std::optional<DrawPrimitive> primitive = gpu::decodePacketPrimitive({command, commandPlaces});
    if (!primitive) {
      return packet;
    }
    drawMode.apply(*primitive);
    primitive->sourceAddress = packet;
    sink.emit(slot, *primitive);
  }
  return std::nullopt;
}

void PacketChainWalker::emitChain(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink) {
  if (const std::optional<std::uint32_t> packet = walk(link, slot, drawMode, sink)) {
    lucent::error("packet-chain",
                  "the packet 0x{:08X} at 0x{:08X} is not a polygon, line or sprite",
                  memory_.mem_r32(*packet + 4u),
                  *packet);
    std::abort();
  }
}

bool PacketChainWalker::tryEmitChain(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink) {
  return !walk(link, slot, drawMode, sink);
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
