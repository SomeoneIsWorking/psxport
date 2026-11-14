// packet_chains.h — the packets a body linked into a tag chain, captured from its first link.
#pragma once

#include "emit_memory.h"
#include "gp0_draw_state.h" // CapturedPacket
#include "gp0_primitive_decode.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::present {

// What a walk may visit; past it the chain is broken and the walk aborts. A revisited packet is refused
// (refuseDrawListCycle) whatever the bound.
struct PacketBound {
  std::uint32_t packetLimit = 0;
  // Bytes 0: packets may lie anywhere.
  GuestSpan arena{};

  // Any number of packets anywhere: only the chain's end or a refused cycle stops the walk.
  static constexpr PacketBound anywhere() {
    return PacketBound{0xFFFFFFFFu, {}};
  }
};

// Follows tag links; link-only packets are skipped. The packet count is shared by every chain one walker visits.
class PacketChainWalker {
public:
  PacketChainWalker(const EmitMemory &memory, const PacketBound &bound) : memory_(memory), bound_(bound) {}

  // Every packet of the chain from `link` that carries words, as it lies in memory now, with the host packets the
  // walk from `link` draws around its nodes, appended to `packets`, its
  // words to `words` and each word's place to `places`.
  void capture(std::uint32_t link,
               std::vector<gpu::CapturedPacket> &packets,
               std::vector<std::uint32_t> &words,
               std::vector<gte::WordPlace> &places);

private:
  // Calls `visit(packet address, PacketView, draw site)` for each packet carrying words, and for each host packet
  // (HostPackets) of the walk from `link` with the address of the node it is drawn around, until the chain ends.
  template <class Visit> void follow(std::uint32_t link, OtSlot slot, Visit &&visit);

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
