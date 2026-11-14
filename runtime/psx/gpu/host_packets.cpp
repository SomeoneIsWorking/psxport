#include "host_packets.h"

#include "blob_codec.h"
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "ordering_table.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iterator>
#include <lucent/log.h>

namespace psx::gpu {

HostPackets::HostPackets() {
  packets_.reserve(kPacketCapacity);
  replacements_.reserve(kPacketCapacity);
  orderReplaced_.reserve(kPacketCapacity);
  words_.reserve(kWordCapacity);
  places_.reserve(kWordCapacity);
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

void HostPackets::insert(
    Core &core, std::uint32_t root, std::uint32_t node, PacketView packet, std::uint32_t site, bool after) {
  if (walking_ || packets_.size() == kPacketCapacity || words_.size() + packet.words.size() > kWordCapacity) {
    refuse(
        core, lucent::format("insert of {} word(s) {}", packet.words.size(), after ? "after" : "before"), root, node);
  }
  packets_.push_back({.root = ramOffset(root),
                      .node = ramOffset(node),
                      .firstWord = static_cast<std::uint32_t>(words_.size()),
                      .wordCount = static_cast<std::uint32_t>(packet.words.size()),
                      .site = site,
                      .after = after});
  words_.insert(words_.end(), packet.words.begin(), packet.words.end());
  if (packet.places.empty()) {
    places_.resize(words_.size());
  } else {
    places_.insert(places_.end(), packet.places.begin(), packet.places.end());
  }
  selected_ = false;
}

void HostPackets::insertBefore(Core &core,
                               std::uint32_t rootGuestAddress,
                               std::uint32_t nodeGuestAddress,
                               std::span<const std::uint32_t> words) {
  insert(core, rootGuestAddress, nodeGuestAddress, PacketView{words, {}}, 0, false);
}

void HostPackets::insertAfter(
    Core &core, std::uint32_t rootGuestAddress, std::uint32_t nodeGuestAddress, PacketView packet, std::uint32_t site) {
  insert(core, rootGuestAddress, nodeGuestAddress, packet, site, true);
}

HostPackets::Scope HostPackets::open() {
  const Scope scope{.held = {.packets = packets_.size(), .replacements = replacements_.size(), .moves = moves_.size()},
                    .words = words_.size(),
                    .enclosing = floor_};
  floor_ = scope.held;
  selected_ = false;
  return scope;
}

void HostPackets::close(const Scope &scope) {
  packets_.resize(scope.held.packets);
  replacements_.resize(scope.held.replacements);
  moves_.resize(scope.held.moves);
  words_.resize(scope.words);
  places_.resize(scope.words);
  floor_ = scope.enclosing;
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
  places_.resize(words_.size());
  selected_ = false;
}

template <typename Drop> void HostPackets::erase(Drop drop) {
  // An enclosing scope's entries stay: its close puts the store back to its own sizes.
  const auto eraseAbove = [&drop](auto &entries, std::size_t floor) {
    entries.erase(std::remove_if(entries.begin() + static_cast<std::ptrdiff_t>(floor),
                                 entries.end(),
                                 [&drop](const auto &entry) {
                                   return drop(entry.root, entry.walked);
                                 }),
                  entries.end());
  };
  eraseAbove(packets_, floor_.packets);
  eraseAbove(replacements_, floor_.replacements);
  eraseAbove(moves_, floor_.moves);
  // The kept packets' and replacements' words move down in storage order, so storage holds only live ones.
  std::uint32_t kept = 0;
  auto packet = packets_.begin();
  auto replacement = replacements_.begin();
  while (packet != packets_.end() || replacement != replacements_.end()) {
    const bool takePacket =
        replacement == replacements_.end() || (packet != packets_.end() && packet->firstWord < replacement->firstWord);
    Packet &entry = takePacket ? *packet++ : *replacement++;
    std::copy_n(words_.begin() + entry.firstWord, entry.wordCount, words_.begin() + kept);
    std::copy_n(places_.begin() + entry.firstWord, entry.wordCount, places_.begin() + kept);
    entry.firstWord = kept;
    kept += entry.wordCount;
  }
  words_.resize(kept);
  places_.resize(kept);
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
    out.u32(packet.site);
    out.boolean(packet.after);
    out.boolean(packet.walked);
  }
}

bool HostPackets::readPackets(platform::BlobReader &in, std::vector<Packet> &packets) {
  packets.resize(std::min<std::size_t>(in.u32(), kPacketCapacity + 1));
  if (packets.size() > kPacketCapacity) {
    return false;
  }
  for (Packet &packet : packets) {
    packet = {.root = in.u32(),
              .node = in.u32(),
              .firstWord = in.u32(),
              .wordCount = in.u32(),
              .site = in.u32(),
              .after = in.boolean(),
              .walked = in.boolean()};
  }
  return true;
}

void HostPackets::save(platform::BlobWriter &out) const {
  writePackets(out, packets_);
  writePackets(out, replacements_);
  out.u32(static_cast<std::uint32_t>(words_.size()));
  for (std::size_t i = 0; i < words_.size(); i++) {
    out.u32(words_[i]);
    const gte::WordPlace &place = places_[i];
    out.boolean(place.has_value());
    if (place) {
      out.u32(std::bit_cast<std::uint32_t>(place->x));
      out.u32(std::bit_cast<std::uint32_t>(place->y));
      out.u32(std::bit_cast<std::uint32_t>(place->z));
      out.u32(std::bit_cast<std::uint32_t>(place->back));
      out.u32(place->model);
    }
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
  std::vector<gte::WordPlace> places(words.size());
  for (std::size_t i = 0; i < words.size(); i++) {
    words[i] = in.u32();
    if (in.boolean()) {
      places[i] = GtePlace{.x = std::bit_cast<float>(in.u32()),
                           .y = std::bit_cast<float>(in.u32()),
                           .z = std::bit_cast<float>(in.u32()),
                           .back = std::bit_cast<float>(in.u32()),
                           .model = in.u32()};
    }
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
  places_.assign(places.begin(), places.end());
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
  for (auto i = static_cast<std::uint32_t>(floor_.packets); i < packets_.size(); i++) {
    if (packets_[i].root == root) {
      order_.push_back(i);
    }
  }
  std::stable_sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return packets_[a].node != packets_[b].node ? packets_[a].node < packets_[b].node
                                                : packets_[a].after < packets_[b].after;
  });
  orderMoves_.clear();
  const auto movesAbove = moves_.begin() + static_cast<std::ptrdiff_t>(floor_.moves);
  std::copy_if(movesAbove, moves_.end(), std::back_inserter(orderMoves_), [root](const Move &move) {
    return move.root == root;
  });
  std::stable_sort(orderMoves_.begin(), orderMoves_.end(), [](const Move &a, const Move &b) {
    return a.node < b.node;
  });
  orderReplaced_.clear();
  for (auto i = static_cast<std::uint32_t>(floor_.replacements); i < replacements_.size(); i++) {
    if (replacements_[i].root == root) {
      orderReplaced_.push_back(i);
    }
  }
  std::sort(orderReplaced_.begin(), orderReplaced_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return replacements_[a].node != replacements_[b].node ? replacements_[a].node < replacements_[b].node : a > b;
  });
}

std::vector<std::uint32_t>::const_iterator HostPackets::first(std::uint32_t node, bool after) const {
  return std::lower_bound(order_.begin(), order_.end(), node, [this, after](std::uint32_t index, std::uint32_t key) {
    const Packet &packet = packets_[index];
    return packet.node != key ? packet.node < key : packet.after < after;
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

void HostPackets::draw(GpuDevice &device, std::uint32_t nodeGuestAddress, bool after) {
  const std::uint32_t node = ramOffset(nodeGuestAddress);
  for (auto at = first(node, after); at != order_.end() && packets_[*at].node == node && packets_[*at].after == after;
       ++at) {
    Packet &packet = packets_[*at];
    if (packet.walked) {
      continue;
    }
    // Drawn as part of the node's packet, so the record classifies it as the node's.
    for (std::uint32_t i = 0; i < packet.wordCount; i++) {
      const gte::WordPlace &place = places_[packet.firstWord + i];
      device.gp0(
          words_[packet.firstWord + i],
          {.packet = nodeGuestAddress,
           .place = place ? std::optional(cpu::NotedPlace{.place = *place, .site = packet.site}) : std::nullopt});
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
