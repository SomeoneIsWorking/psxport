#include "ordering_table.h"

#include "core.h"
#include "gpu_device.h"

namespace psx::gpu {

namespace {
// Read one node's header word out of guest main RAM. The single RAM touch of this owner: every other
// function here works on a value that came through here, so there is one place a masking or alignment
// change could be wrong.
OtNode readNode(Core &core, std::uint32_t mainRamOffset) {
  return OtNode(mainRamOffset, core.mem_r32(mainRamOffset));
}
} // namespace

OrderingTableCursor::OrderingTableCursor(Core &core, std::uint32_t rootGuestAddress)
    : core_(core), node_(readNode(core, mainRamOffsetOf(rootGuestAddress))) {
  nodesEntered_ = 1;
}

bool OrderingTableCursor::advance() {
  if (node_.endsChain()) {
    return false;
  }
  // The CAP is a refusal, not a length. A well-formed table never reaches it — a DrawOTag is bounded by
  // the packet pool — so hitting it means the chain is cyclic or malformed, and the honest thing is to
  // stop and record that. Recorded separately from the end-of-chain case because the two mean opposite
  // things, and a caller that cannot tell them apart reads a hung frame as an empty table.
  if (nodesEntered_ >= kOtNodeLimit) {
    truncated_ = true;
    return false;
  }
  node_ = readNode(core_, node_.nextRamOffset());
  nodesEntered_++;
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
  assigned_[mainRamOffsetOf(packetGuestAddress)] = slot;
}

void OtTables::clear() {
  areas_.clear();
  assigned_.clear();
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

std::optional<present::OtSlot> OtTables::assigned(std::uint32_t packetGuestAddress) const {
  const auto found = assigned_.find(mainRamOffsetOf(packetGuestAddress));
  if (found == assigned_.end()) {
    return std::nullopt;
  }
  return found->second;
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
}

void submitOrderingTable(Core &core, GpuDevice &device, std::uint32_t rootGuestAddress) {
  OrderingTableCursor walk(core, rootGuestAddress);
  const OtTables &tables = core.otTables;
  device.enterSlot(std::nullopt, true);
  do {
    const OtNode &node = walk.node();
    std::optional<present::OtSlot> slot = tables.slotOf(node.guestAddress());
    if (!slot && node.carriesPrimitive()) {
      slot = tables.assigned(node.guestAddress());
    }
    if (slot) {
      device.enterSlot(slot, tables.descending(slot->table));
    }
    if (!node.carriesPrimitive()) {
      continue;
    }
    const std::optional<present::RecordKey> key = core.emission.keyFor(node.guestAddress());
    device.beginPacket();
    for (unsigned i = 0; i < node.gp0WordCount(); i++) {
      const std::uint32_t offset = node.gp0WordRamOffset(i);
      device.gp0(core.mem_r32(offset), guestAddressOf(offset), key);
    }
  } while (walk.advance());
  device.enterSlot(std::nullopt, true);
  core.otTables.endWalk();
}

} // namespace psx::gpu
