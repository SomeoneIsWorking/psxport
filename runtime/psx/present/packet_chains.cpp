#include "packet_chains.h"

#include "draw_list_cycle.h"
#include "guest_memory_map.h"
#include "ordering_table.h"

#include <lucent/log.h>

#include <cstdlib>
#include <span>

namespace psx::present {

template <class Visit>
std::optional<std::uint32_t> PacketChainWalker::follow(std::uint32_t link, OtSlot slot, Visit &&visit) {
  gpu::LinkCycleCheck cycle;
  for (gpu::OtNode tag(0, gpu::OtHeaderWord{link}); !tag.endsChain();) {
    const std::uint32_t packet = gpu::guestAddressOf(tag.nextRamOffset());
    if (!cycle.enter(gpu::mainRamOffsetOf(packet))) {
      gpu::refuseDrawListCycle(
          memory_.core(),
          packet,
          "present/packet_chains.cpp:PacketChainWalker::follow",
          lucent::format("the chain from link 0x{:08X} into slot {}:{} returns to it after {} packets",
                         link,
                         slot.table,
                         slot.index,
                         walked_));
      return std::nullopt;
    }
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
    if (!visit(packet, gpu::PacketView{command, commandPlaces})) {
      return packet;
    }
  }
  return std::nullopt;
}

void PacketChainWalker::capture(std::uint32_t link,
                                std::vector<gpu::CapturedPacket> &packets,
                                std::vector<std::uint32_t> &words,
                                std::vector<gte::WordPlace> &places) {
  (void)follow(link, OtSlot{}, [&](std::uint32_t packet, gpu::PacketView view) {
    packets.push_back({.address = packet,
                       .offset = static_cast<std::uint32_t>(words.size()),
                       .count = static_cast<std::uint32_t>(view.words.size()),
                       .site = memory_.placingSite(packet + 4u, view.words)});
    words.insert(words.end(), view.words.begin(), view.words.end());
    places.insert(places.end(), view.places.begin(), view.places.end());
    return true;
  });
}

} // namespace psx::present
