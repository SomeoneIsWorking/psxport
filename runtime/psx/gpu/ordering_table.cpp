#include "ordering_table.h"

#include "core.h"
#include "game.h"
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
  OrderingTableCursor walk(core, rootGuestAddress);
  device.noteDrawEntry(static_cast<std::uint32_t>(core.game->timing.displayField()));
  core.otTables.beginWalk();
  const OtTables &tables = core.otTables;
  device.enterSlot(std::nullopt, true);
  do {
    // A walk is milliseconds of host work inside one executor segment; a present due meanwhile lands now.
    core.game->display.showIfDue();
    const OtNode &node = walk.node();
    std::optional<present::OtSlot> slot = tables.slotOf(node.guestAddress());
    if (!slot && node.carriesPrimitive()) {
      slot = tables.assigned(node.guestAddress());
    }
    if (slot) {
      device.enterSlot(slot, tables.descending(slot->table));
    }
    if (node.carriesPrimitive()) {
      const std::optional<present::RecordKey> key = core.emission.keyFor(node.guestAddress());
      // The command word's owner, and the other side of an overwrite pair in the packet: the writer that stored
      // over an owned packet's word, or the scope an unowned packet's writer stored over.
      const present::OwnerHandle owner = core.owners.ownerOf(node.guestAddress() + 4u);
      const bool owned = present::OwnerLedger::isScope(owner);
      present::OwnerHandle mixed = present::kNoOwner;
      for (unsigned i = 0; i < node.gp0WordCount() && mixed == present::kNoOwner; i++) {
        const std::uint32_t address = guestAddressOf(node.gp0WordRamOffset(i));
        const present::OwnerHandle replaced = core.owners.overwrote(address);
        if (replaced != present::kNoOwner) {
          mixed = owned ? core.owners.ownerOf(address) : replaced;
        }
      }
      device.beginPacket();
      for (unsigned i = 0; i < node.gp0WordCount(); i++) {
        const std::uint32_t offset = node.gp0WordRamOffset(i);
        device.gp0(core.mem_r32(offset),
                   {.sourceAddress = guestAddressOf(offset), .packetKey = key, .owner = owner, .mixedOwner = mixed});
      }
    }
    if (const std::optional<present::OtSlot> empty = tables.emptyAfter(node.guestAddress())) {
      device.enterSlot(empty, tables.descending(empty->table));
    }
  } while (walk.advance());
  device.enterSlot(std::nullopt, true);
  core.otTables.endWalk();
  core.owners.drawListSubmitted();
}

} // namespace psx::gpu
