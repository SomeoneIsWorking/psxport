#include "host_packets.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "ordering_table.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

namespace psx::gpu {

HostPackets::HostPackets() {
  packets_.reserve(kPacketCapacity);
  words_.reserve(kWordCapacity);
  walkOrder_.reserve(kPacketCapacity);
}

void HostPackets::insertBefore(Core &core,
                               std::uint32_t rootGuestAddress,
                               std::uint32_t nodeGuestAddress,
                               std::span<const std::uint32_t> words) {
  if (walking_ || packets_.size() == kPacketCapacity || words_.size() + words.size() > kWordCapacity) {
    lucent::error("host-packets",
                  "insert of {} word(s) before node 0x{:08X} of table 0x{:08X} refused at field {}: {} of {} "
                  "packets and {} of {} words held{}",
                  words.size(),
                  nodeGuestAddress,
                  rootGuestAddress,
                  core.game->timing.displayField(),
                  packets_.size(),
                  kPacketCapacity,
                  words_.size(),
                  kWordCapacity,
                  walking_ ? ", inside a walk" : "");
    std::abort();
  }
  packets_.push_back({.root = mainRamOffsetOf(rootGuestAddress),
                      .node = mainRamOffsetOf(nodeGuestAddress),
                      .firstWord = static_cast<std::uint32_t>(words_.size()),
                      .wordCount = static_cast<std::uint32_t>(words.size())});
  words_.insert(words_.end(), words.begin(), words.end());
}

void HostPackets::discard(std::uint32_t rootGuestAddress) {
  const std::uint32_t root = mainRamOffsetOf(rootGuestAddress);
  std::erase_if(packets_, [root](const Packet &packet) {
    return packet.root == root;
  });
  // The other roots' words move down in order, so storage holds only live packets.
  std::uint32_t kept = 0;
  for (Packet &packet : packets_) {
    std::copy_n(words_.begin() + packet.firstWord, packet.wordCount, words_.begin() + kept);
    packet.firstWord = kept;
    kept += packet.wordCount;
  }
  words_.resize(kept);
}

bool HostPackets::beginWalk(std::uint32_t rootGuestAddress) {
  walkRoot_ = mainRamOffsetOf(rootGuestAddress);
  walkOrder_.clear();
  for (std::uint32_t i = 0; i < packets_.size(); i++) {
    if (packets_[i].root == walkRoot_) {
      walkOrder_.push_back(i);
    }
  }
  std::stable_sort(walkOrder_.begin(), walkOrder_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return packets_[a].node < packets_[b].node;
  });
  walkPending_ = walkOrder_.size();
  walking_ = walkPending_ != 0;
  return walking_;
}

void HostPackets::drawBefore(GpuDevice &device, std::uint32_t nodeGuestAddress) {
  const std::uint32_t node = mainRamOffsetOf(nodeGuestAddress);
  auto at =
      std::lower_bound(walkOrder_.begin(), walkOrder_.end(), node, [this](std::uint32_t index, std::uint32_t key) {
        return packets_[index].node < key;
      });
  for (; at != walkOrder_.end() && packets_[*at].node == node; ++at) {
    Packet &packet = packets_[*at];
    if (packet.drawn) {
      continue;
    }
    for (std::uint32_t i = 0; i < packet.wordCount; i++) {
      device.gp0(words_[packet.firstWord + i]);
    }
    packet.drawn = true;
    walkPending_--;
  }
}

void HostPackets::endWalk(Core &core) {
  if (!walking_) {
    return;
  }
  walking_ = false;
  if (walkPending_ != 0) {
    const auto lost = std::find_if(packets_.begin(), packets_.end(), [this](const Packet &packet) {
      return packet.root == walkRoot_ && !packet.drawn;
    });
    lucent::error("host-packets",
                  "the walk from 0x{:08X} at field {} never entered node 0x{:08X}: {} packet(s) inserted before "
                  "nodes off its chain",
                  guestAddressOf(walkRoot_),
                  core.game->timing.displayField(),
                  guestAddressOf(lost->node),
                  walkPending_);
    std::abort();
  }
  discard(guestAddressOf(walkRoot_));
}

} // namespace psx::gpu
