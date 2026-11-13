#include "ordering_table.h"

#include "core.h"
#include "game.h"
#include "gp0_primitive_decode.h"
#include "gpu_device.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace psx::gpu {

namespace {

OtNode readNode(Core &core, std::uint32_t mainRamOffset) {
  return OtNode(mainRamOffset, OtHeaderWord{core.mem_r32(mainRamOffset)});
}

[[noreturn]] void refuseReplacement(Core &core, const OtNode &node, std::size_t words) {
  lucent::error("host-packets",
                "node 0x{:08X} at field {} holds {} word(s) and was replaced by {}",
                node.guestAddress(),
                core.game->timing.displayField(),
                node.gp0WordCount(),
                words);
  std::abort();
}

[[noreturn]] void refuseUnmovable(Core &core, const OtNode &node) {
  lucent::error("host-packets",
                "node 0x{:08X} at field {} was given a move, but its {} word(s) hold a poly-line or a cut command",
                node.guestAddress(),
                core.game->timing.displayField(),
                node.gp0WordCount());
  std::abort();
}

} // namespace

OrderingTableCursor::OrderingTableCursor(Core &core, std::uint32_t rootGuestAddress)
    : core_(core), node_(readNode(core, mainRamOffsetOf(rootGuestAddress))) {
  nodesEntered_ = 1;
  cycle_.enter(node_.ramOffset());
}

bool OrderingTableCursor::advance() {
  if (node_.endsChain()) {
    return false;
  }
  const OtNode from = node_;
  node_ = readNode(core_, from.nextRamOffset());
  nodesEntered_++;
  if (!cycle_.enter(node_.ramOffset())) {
    refuseDrawListCycle(core_,
                        node_.guestAddress(),
                        "gpu/ordering_table.cpp:OrderingTableCursor::advance",
                        lucent::format("node 0x{:08X} links back to it after {} nodes; {}",
                                       from.guestAddress(),
                                       nodesEntered_,
                                       guestWriterOf(core_, from.guestAddress())));
    return false;
  }
  return true;
}

OrderingTableWalk::OrderingTableWalk(Core &core, std::uint32_t rootGuestAddress)
    : core_(core), hostPackets_(core.hostPackets), cursor_(core, rootGuestAddress) {
  hostPackets_.select(rootGuestAddress);
  readNode();
}

void OrderingTableWalk::readNode() {
  const OtNode &node = cursor_.node();
  const std::span<const std::uint32_t> replaced =
      node.carriesPrimitive() ? hostPackets_.replaced(node.guestAddress()) : std::span<const std::uint32_t>{};
  if (!replaced.empty() && replaced.size() != node.gp0WordCount()) {
    refuseReplacement(core_, node, replaced.size());
  }
  for (unsigned i = 0; i < node.gp0WordCount(); i++) {
    words_[i] = replaced.empty() ? core_.mem_r32(node.gp0WordRamOffset(i)) : replaced[i];
  }
  movedX_ = node.carriesPrimitive() ? hostPackets_.movedX(node.guestAddress()) : 0;
  if (movedX_ != 0 && !moveVerticesX(std::span(words_.data(), node.gp0WordCount()), movedX_)) {
    refuseUnmovable(core_, node);
  }
}

bool OrderingTableWalk::advance() {
  if (!cursor_.advance()) {
    return false;
  }
  readNode();
  return true;
}

void OtTables::name(
    std::uint16_t table, std::uint32_t baseGuestAddress, std::uint32_t count, std::uint32_t strideBytes, OtWalk walk) {
  const Area area{table, mainRamOffsetOf(baseGuestAddress), count, strideBytes, walk};
  for (Area &named : areas_) {
    if (named.base == area.base) {
      named = area;
      return;
    }
  }
  areas_.push_back(area);
}

void OtTables::assign(std::uint32_t packetGuestAddress, present::OtSlot slot) {
  assigned_.push_back({.offset = mainRamOffsetOf(packetGuestAddress),
                       .order = static_cast<std::uint32_t>(assigned_.size()),
                       .slot = slot});
  sorted_ = false;
}

void OtTables::placeEmpty(present::OtSlot slot, std::uint32_t packetGuestAddress) {
  empties_.push_back({.offset = mainRamOffsetOf(packetGuestAddress), .slot = slot});
}

std::optional<present::OtSlot> OtTables::emptyAfter(std::uint32_t packetGuestAddress) const {
  const std::uint32_t offset = mainRamOffsetOf(packetGuestAddress);
  const auto empty = std::find_if(empties_.begin(), empties_.end(), [offset](const Assigned &entry) {
    return entry.offset == offset;
  });
  return empty != empties_.end() ? std::optional(empty->slot) : std::nullopt;
}

void OtTables::clear() {
  areas_.clear();
  assigned_.clear();
  empties_.clear();
  sorted_ = true;
}

std::optional<present::OtSlot> OtTables::slotOf(std::uint32_t nodeGuestAddress) const {
  const std::uint32_t offset = mainRamOffsetOf(nodeGuestAddress);
  for (const Area &area : areas_) {
    if (offset >= area.base && offset < area.base + area.count * area.stride &&
        (offset - area.base) % area.stride == 0) {
      return present::OtSlot{area.table, (offset - area.base) / area.stride};
    }
  }
  return std::nullopt;
}

void OtTables::beginWalk() {
  // A packet's last assignment ends its run.
  std::sort(assigned_.begin(), assigned_.end(), [](const Assigned &a, const Assigned &b) {
    return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
  });
  sorted_ = true;
}

std::optional<present::OtSlot> OtTables::assigned(std::uint32_t packetGuestAddress) const {
  if (!sorted_) {
    lucent::error("ot", "an assigned bucket was asked for outside a walk");
    std::abort();
  }
  const std::uint32_t offset = mainRamOffsetOf(packetGuestAddress);
  const auto after =
      std::upper_bound(assigned_.begin(), assigned_.end(), offset, [](std::uint32_t key, const Assigned &entry) {
        return key < entry.offset;
      });
  if (after == assigned_.begin() || std::prev(after)->offset != offset) {
    return std::nullopt;
  }
  return std::prev(after)->slot;
}

bool OtTables::descending(std::uint16_t table) const {
  for (const Area &area : areas_) {
    if (area.table == table) {
      return area.walk == OtWalk::HighToLow;
    }
  }
  return true;
}

void OtTables::endWalk() {
  assigned_.clear();
  empties_.clear();
  sorted_ = true;
}

void submitOrderingTable(Core &core, GpuDevice &device, std::uint32_t rootGuestAddress) {
  core.hostPackets.beginWalk(rootGuestAddress);
  OrderingTableWalk walk(core, rootGuestAddress);
  device.noteDrawEntry(static_cast<std::uint32_t>(core.game->timing.displayField()));
  core.otTables.beginWalk();
  const OtTables &tables = core.otTables;
  device.enterSlot(std::nullopt, true);
  device.beginChain(rootGuestAddress);
  do {
    // A walk is milliseconds of host work inside one executor segment; a present due meanwhile lands now.
    core.game->display.showIfDue();
    const OtNode &node = walk.node();
    // Drawn in the bucket the previous node left open, as a guest link before this node would be.
    core.hostPackets.drawBefore(device, node.guestAddress());
    std::optional<present::OtSlot> slot = tables.slotOf(node.guestAddress());
    if (!slot && node.carriesPrimitive()) {
      slot = tables.assigned(node.guestAddress());
    }
    if (slot) {
      device.enterSlot(slot, tables.descending(slot->table));
    }
    if (node.carriesPrimitive()) {
      // The packet is its command word's writer's.
      const present::OwnerHandle owner = core.owners.ownerOf(node.guestAddress() + 4u);
      const std::span<const std::uint32_t> words = walk.words();
      for (unsigned i = 0; i < words.size(); i++) {
        // A moved vertex is no longer where its draw site stored it, so it keeps no place.
        const std::uint32_t address = guestAddressOf(node.gp0WordRamOffset(i));
        device.gp0(words[i],
                   {.sourceAddress = address,
                    .packet = node.guestAddress(),
                    .owner = owner,
                    .place = core.vertexPlaces.at(address, words[i])});
      }
    }
    if (const std::optional<present::OtSlot> empty = tables.emptyAfter(node.guestAddress())) {
      device.enterSlot(empty, tables.descending(empty->table));
    }
  } while (walk.advance());
  device.endChain();
  device.enterSlot(std::nullopt, true);
  core.otTables.endWalk();
  core.hostPackets.endWalk(core);
}

} // namespace psx::gpu
