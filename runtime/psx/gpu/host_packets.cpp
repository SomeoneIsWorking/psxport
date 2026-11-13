#include "host_packets.h"

#include "blob_codec.h"
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "ordering_table.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <lucent/log.h>

namespace psx::gpu {

HostPackets::HostPackets() {
  packets_.reserve(kPacketCapacity);
  replacements_.reserve(kPacketCapacity);
  orderReplaced_.reserve(kPacketCapacity);
  words_.reserve(kWordCapacity);
  moves_.reserve(kMoveCapacity);
  order_.reserve(kPacketCapacity);
  orderMoves_.reserve(kMoveCapacity);
}

std::uint32_t HostPackets::ramOffset(std::uint32_t guestAddress) {
  return mainRamOffsetOf(guestAddress);
}

void HostPackets::refuse(Core &core, std::string_view what, std::uint32_t root, std::uint32_t node) const {
  lucent::error(
      "host-packets",
      "{} node 0x{:08X} of table 0x{:08X} refused at field {}: {} of {} packets, {} of {} replacements, {} of "
      "{} words and {} of {} moves held{}",
      what,
      node,
      root,
      core.game->timing.displayField(),
      packets_.size(),
      kPacketCapacity,
      replacements_.size(),
      kPacketCapacity,
      words_.size(),
      kWordCapacity,
      moves_.size(),
      kMoveCapacity,
      walking_ ? ", inside a walk" : "");
  std::abort();
}

void HostPackets::insertBefore(Core &core,
                               std::uint32_t rootGuestAddress,
                               std::uint32_t nodeGuestAddress,
                               std::span<const std::uint32_t> words) {
  if (walking_ || packets_.size() == kPacketCapacity || words_.size() + words.size() > kWordCapacity) {
    refuse(core, lucent::format("insert of {} word(s) before", words.size()), rootGuestAddress, nodeGuestAddress);
  }
  packets_.push_back({.root = ramOffset(rootGuestAddress),
                      .node = ramOffset(nodeGuestAddress),
                      .firstWord = static_cast<std::uint32_t>(words_.size()),
                      .wordCount = static_cast<std::uint32_t>(words.size())});
  words_.insert(words_.end(), words.begin(), words.end());
  selected_ = false;
}

void HostPackets::moveX(Core &core, std::uint32_t rootGuestAddress, std::uint32_t nodeGuestAddress, int dx) {
  if (walking_ || moves_.size() == kMoveCapacity) {
    refuse(core, lucent::format("move by {} of", dx), rootGuestAddress, nodeGuestAddress);
  }
  moves_.push_back({.root = ramOffset(rootGuestAddress), .node = ramOffset(nodeGuestAddress), .dx = dx});
  selected_ = false;
}

void HostPackets::replace(Core &core,
                          std::uint32_t rootGuestAddress,
                          std::uint32_t nodeGuestAddress,
                          std::span<const std::uint32_t> words) {
  if (walking_ || replacements_.size() == kPacketCapacity || words_.size() + words.size() > kWordCapacity) {
    refuse(core, lucent::format("replacement by {} word(s) of", words.size()), rootGuestAddress, nodeGuestAddress);
  }
  replacements_.push_back({.root = ramOffset(rootGuestAddress),
                           .node = ramOffset(nodeGuestAddress),
                           .firstWord = static_cast<std::uint32_t>(words_.size()),
                           .wordCount = static_cast<std::uint32_t>(words.size())});
  words_.insert(words_.end(), words.begin(), words.end());
  selected_ = false;
}

template <typename Drop> void HostPackets::erase(Drop drop) {
  std::erase_if(packets_, [&drop](const Packet &packet) {
    return drop(packet.root, packet.walked);
  });
  std::erase_if(replacements_, [&drop](const Packet &packet) {
    return drop(packet.root, packet.walked);
  });
  std::erase_if(moves_, [&drop](const Move &move) {
    return drop(move.root, move.walked);
  });
  // The kept packets' and replacements' words move down in storage order, so storage holds only live ones.
  std::uint32_t kept = 0;
  auto packet = packets_.begin();
  auto replacement = replacements_.begin();
  while (packet != packets_.end() || replacement != replacements_.end()) {
    const bool takePacket =
        replacement == replacements_.end() || (packet != packets_.end() && packet->firstWord < replacement->firstWord);
    Packet &entry = takePacket ? *packet++ : *replacement++;
    std::copy_n(words_.begin() + entry.firstWord, entry.wordCount, words_.begin() + kept);
    entry.firstWord = kept;
    kept += entry.wordCount;
  }
  words_.resize(kept);
  selected_ = false;
}

void HostPackets::discard(std::uint32_t rootGuestAddress) {
  const std::uint32_t root = ramOffset(rootGuestAddress);
  erase([root](std::uint32_t entryRoot, bool /*walked*/) {
    return entryRoot == root;
  });
}

void HostPackets::writePackets(platform::BlobWriter &out, std::span<const Packet> packets) {
  out.u32(static_cast<std::uint32_t>(packets.size()));
  for (const Packet &packet : packets) {
    out.u32(packet.root);
    out.u32(packet.node);
    out.u32(packet.firstWord);
    out.u32(packet.wordCount);
    out.boolean(packet.walked);
  }
}

bool HostPackets::readPackets(platform::BlobReader &in, std::vector<Packet> &packets) {
  packets.resize(std::min<std::size_t>(in.u32(), kPacketCapacity + 1));
  if (packets.size() > kPacketCapacity) {
    return false;
  }
  for (Packet &packet : packets) {
    packet = {.root = in.u32(), .node = in.u32(), .firstWord = in.u32(), .wordCount = in.u32(), .walked = in.boolean()};
  }
  return true;
}

void HostPackets::save(platform::BlobWriter &out) const {
  writePackets(out, packets_);
  writePackets(out, replacements_);
  out.u32(static_cast<std::uint32_t>(words_.size()));
  for (const std::uint32_t word : words_) {
    out.u32(word);
  }
  out.u32(static_cast<std::uint32_t>(moves_.size()));
  for (const Move &move : moves_) {
    out.u32(move.root);
    out.u32(move.node);
    out.i32(move.dx);
    out.boolean(move.walked);
  }
}

bool HostPackets::load(platform::BlobReader &in, std::string &error) {
  std::vector<Packet> packets;
  std::vector<Packet> replacements;
  if (!readPackets(in, packets) || !readPackets(in, replacements)) {
    error = lucent::format("host packets: more than the {} packets or replacements held", kPacketCapacity);
    return false;
  }
  std::vector<std::uint32_t> words(std::min<std::size_t>(in.u32(), kWordCapacity + 1));
  if (words.size() > kWordCapacity) {
    error = lucent::format("host packets: more than the {} words held", kWordCapacity);
    return false;
  }
  for (std::uint32_t &word : words) {
    word = in.u32();
  }
  std::vector<Move> moves(std::min<std::size_t>(in.u32(), kMoveCapacity + 1));
  if (moves.size() > kMoveCapacity) {
    error = lucent::format("host packets: more than the {} moves held", kMoveCapacity);
    return false;
  }
  for (Move &move : moves) {
    move = {.root = in.u32(), .node = in.u32(), .dx = in.i32(), .walked = in.boolean()};
  }
  if (!in.ok()) {
    error = "host packets: the stream ends early";
    return false;
  }
  for (const std::vector<Packet> *entries : {&packets, &replacements}) {
    for (const Packet &packet : *entries) {
      if (packet.firstWord > words.size() || packet.wordCount > words.size() - packet.firstWord) {
        error = lucent::format("host packets: a packet at node 0x{:08X} reaches past the {} words",
                               guestAddressOf(packet.node),
                               words.size());
        return false;
      }
    }
  }
  // Assigned into the reserved storage, which never reallocates.
  packets_.assign(packets.begin(), packets.end());
  replacements_.assign(replacements.begin(), replacements.end());
  words_.assign(words.begin(), words.end());
  moves_.assign(moves.begin(), moves.end());
  selected_ = false;
  return true;
}

void HostPackets::select(std::uint32_t rootGuestAddress) {
  const std::uint32_t root = ramOffset(rootGuestAddress);
  if (selected_ && selectedRoot_ == root) {
    return;
  }
  selectedRoot_ = root;
  selected_ = true;
  order_.clear();
  for (std::uint32_t i = 0; i < packets_.size(); i++) {
    if (packets_[i].root == root) {
      order_.push_back(i);
    }
  }
  std::stable_sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return packets_[a].node < packets_[b].node;
  });
  orderMoves_.clear();
  std::copy_if(moves_.begin(), moves_.end(), std::back_inserter(orderMoves_), [root](const Move &move) {
    return move.root == root;
  });
  std::stable_sort(orderMoves_.begin(), orderMoves_.end(), [](const Move &a, const Move &b) {
    return a.node < b.node;
  });
  orderReplaced_.clear();
  for (std::uint32_t i = 0; i < replacements_.size(); i++) {
    if (replacements_[i].root == root) {
      orderReplaced_.push_back(i);
    }
  }
  std::sort(orderReplaced_.begin(), orderReplaced_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return replacements_[a].node != replacements_[b].node ? replacements_[a].node < replacements_[b].node : a > b;
  });
}

std::vector<std::uint32_t>::const_iterator HostPackets::firstBefore(std::uint32_t nodeGuestAddress) const {
  return std::lower_bound(
      order_.begin(), order_.end(), ramOffset(nodeGuestAddress), [this](std::uint32_t index, std::uint32_t key) {
        return packets_[index].node < key;
      });
}

int HostPackets::movedX(std::uint32_t nodeGuestAddress) const {
  const std::uint32_t node = ramOffset(nodeGuestAddress);
  auto at = std::lower_bound(orderMoves_.begin(), orderMoves_.end(), node, [](const Move &move, std::uint32_t key) {
    return move.node < key;
  });
  int dx = 0;
  for (; at != orderMoves_.end() && at->node == node; ++at) {
    dx += at->dx;
  }
  return dx;
}

std::span<const std::uint32_t> HostPackets::replaced(std::uint32_t nodeGuestAddress) const {
  const std::uint32_t node = ramOffset(nodeGuestAddress);
  const auto at = std::lower_bound(
      orderReplaced_.begin(), orderReplaced_.end(), node, [this](std::uint32_t index, std::uint32_t key) {
        return replacements_[index].node < key;
      });
  if (at == orderReplaced_.end() || replacements_[*at].node != node) {
    return {};
  }
  const Packet &replacement = replacements_[*at];
  return {words_.data() + replacement.firstWord, replacement.wordCount};
}

void HostPackets::beginWalk(std::uint32_t rootGuestAddress) {
  erase([](std::uint32_t /*root*/, bool walked) {
    return walked;
  });
  select(rootGuestAddress);
  walkPending_ = order_.size();
  walking_ = true;
}

void HostPackets::drawBefore(GpuDevice &device, std::uint32_t nodeGuestAddress) {
  const std::uint32_t node = ramOffset(nodeGuestAddress);
  for (auto at = firstBefore(nodeGuestAddress); at != order_.end() && packets_[*at].node == node; ++at) {
    Packet &packet = packets_[*at];
    if (packet.walked) {
      continue;
    }
    for (std::uint32_t i = 0; i < packet.wordCount; i++) {
      device.gp0(words_[packet.firstWord + i]);
    }
    packet.walked = true;
    walkPending_--;
  }
}

void HostPackets::endWalk(Core &core) {
  walking_ = false;
  if (walkPending_ != 0) {
    const auto lost = std::find_if(packets_.begin(), packets_.end(), [this](const Packet &packet) {
      return packet.root == selectedRoot_ && !packet.walked;
    });
    lucent::error("host-packets",
                  "the walk from 0x{:08X} at field {} never entered node 0x{:08X}: {} packet(s) inserted before "
                  "nodes off its chain",
                  guestAddressOf(selectedRoot_),
                  core.game->timing.displayField(),
                  guestAddressOf(lost->node),
                  walkPending_);
    std::abort();
  }
  for (Move &move : moves_) {
    if (move.root == selectedRoot_) {
      move.walked = true;
    }
  }
  for (Packet &replacement : replacements_) {
    if (replacement.root == selectedRoot_) {
      replacement.walked = true;
    }
  }
}

} // namespace psx::gpu
