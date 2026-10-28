// packet_chains.h — packets a body linked into tag chains, read back as record primitives: one chain from its
// first packet, or the chains of a table of bucket heads.
#pragma once

#include "emit_memory.h"
#include "gp0_primitive_decode.h"
#include "state_producer.h"

#include <array>
#include <cstdint>
#include <optional>

namespace psx::present {

// What a walk may visit; past it the chain is broken and the walk aborts. A revisited packet is a record fault
// whatever the bound.
struct PacketBound {
  std::uint32_t packetLimit = 0;
  // Bytes 0: packets may lie anywhere.
  GuestSpan arena{};

  static constexpr PacketBound count(std::uint32_t packets) {
    return PacketBound{packets, {}};
  }
  // Any number of packets anywhere: only the chain's end or a cycle stops the walk.
  static constexpr PacketBound anywhere() {
    return PacketBound{0xFFFFFFFFu, {}};
  }
  // Every packet and its words inside `arena`, which holds at most one packet per word.
  static constexpr PacketBound within(GuestSpan arena) {
    return PacketBound{arena.bytes / 4u + 1u, arena};
  }
};

// `buckets` heads `stride` bytes apart from `base`, each word at `headOffset` linking the bucket's first
// packet; bucket `i` is slot `first.index + i` of table `first.table`. Walked from the last bucket to the first.
struct BucketHeads {
  std::uint32_t base = 0;
  std::uint32_t buckets = 0;
  OtSlot first;
  std::uint32_t stride = 4;
  std::uint32_t headOffset = 0;
};

// Follows tag links, decoding each packet. Link-only and no-op packets are skipped, and so are draw environment
// packets other than the draw mode: a composed primitive takes the environment in effect where it lands in the record.
// The packet count is shared by every chain one walker visits.
class PacketChainWalker {
public:
  PacketChainWalker(const EmitMemory &memory, const PacketBound &bound) : memory_(memory), bound_(bound) {}

  // The chain whose first link is `link` (a head or tag word; its low 24 bits name the packet), into `slot`.
  // Aborts at a packet that is not a polygon, line or sprite.
  void emitChain(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink);
  // The same chain; false, having emitted the packets before it, at a packet that is not a polygon, line or sprite.
  [[nodiscard]] bool tryEmitChain(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink);
  // Every bucket's chain under one draw mode.
  void emitBuckets(const BucketHeads &heads, DrawModeTracker &drawMode, PrimitiveSink &sink);

private:
  // The address of the first packet that is not a polygon, line or sprite, or nullopt when the chain ends.
  std::optional<std::uint32_t> walk(std::uint32_t link, OtSlot slot, DrawModeTracker &drawMode, PrimitiveSink &sink);

  // A tag's 8-bit count bounds a packet.
  static constexpr std::size_t kMaxPacketWords = 255;

  EmitMemory memory_;
  PacketBound bound_;
  std::uint32_t walked_ = 0;
  // One packet's words and their places, held across chains so a walk does not construct them per bucket.
  std::array<std::uint32_t, kMaxPacketWords> words_{};
  std::array<gte::WordPlace, kMaxPacketWords> places_{};
};

} // namespace psx::present
