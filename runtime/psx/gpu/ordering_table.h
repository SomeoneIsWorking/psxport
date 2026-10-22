// ordering_table.h — the guest's DrawOTag linked list: node headers, the walk over guest RAM, and the
// named tables a walk attributes packets to.
#pragma once

#include "frame_record.h"
#include "guest_memory_map.h"

#include <cstdint>
#include <optional>
#include <vector>

class Core;

namespace psx::gpu {

class GpuDevice;

// A node header's 24-bit next field; all ones and zero both end the chain.
inline constexpr std::uint32_t kOtNextAddressMask = 0xFFFFFFu;
inline constexpr std::uint32_t kOtChainEnd = kOtNextAddressMask;
inline constexpr std::uint32_t kOtChainEndZero = 0;
// A node address: word-aligned within the 2 MB main-RAM window.
inline constexpr std::uint32_t kMainRamOffsetMask = 0x1FFFFCu;
inline constexpr std::uint32_t kOtHeaderBytes = 4;
// More nodes than any packet pool holds: the chain is cyclic or malformed.
inline constexpr int kOtNodeLimit = 0x10000;

constexpr std::uint32_t guestAddressOf(std::uint32_t mainRamOffset) {
  return kKseg0Base | (mainRamOffset & kMainRamOffsetMask);
}
constexpr std::uint32_t mainRamOffsetOf(std::uint32_t guestAddress) {
  return guestAddress & kMainRamOffsetMask;
}

// Header bits 31-24: the node's GP0 word count (0 is a link-only node); 23-0: the next node's offset.
enum class OtHeaderWord : std::uint32_t {};

class OtNode {
public:
  constexpr OtNode() = default;
  constexpr OtNode(std::uint32_t mainRamOffset, OtHeaderWord headerWord)
      : ramOffset_(mainRamOffset), header_(static_cast<std::uint32_t>(headerWord)) {}

  constexpr std::uint32_t ramOffset() const {
    return ramOffset_;
  }
  constexpr std::uint32_t guestAddress() const {
    return guestAddressOf(ramOffset_);
  }
  constexpr std::uint32_t headerWord() const {
    return header_;
  }
  constexpr unsigned gp0WordCount() const {
    return header_ >> 24;
  }
  constexpr bool carriesPrimitive() const {
    return gp0WordCount() != 0;
  }
  // GP0 word `index` of this node's packet; word 0 follows the header.
  constexpr std::uint32_t gp0WordRamOffset(unsigned index) const {
    return ramOffset_ + kOtHeaderBytes + kOtHeaderBytes * index;
  }
  constexpr std::uint32_t nextAddressField() const {
    return header_ & kOtNextAddressMask;
  }
  constexpr std::uint32_t nextRamOffset() const {
    return nextAddressField() & kMainRamOffsetMask;
  }
  // Tested on the unmasked field: 0xFFFFFF narrows to a plausible 0x1FFFFC.
  constexpr bool endsChain() const {
    const std::uint32_t next = nextAddressField();
    return next == kOtChainEnd || next == kOtChainEndZero;
  }

private:
  std::uint32_t ramOffset_ = 0;
  std::uint32_t header_ = 0;
};

// One DrawOTag walked through guest RAM; stops at the chain's end or at kOtNodeLimit nodes.
class OrderingTableCursor {
public:
  explicit OrderingTableCursor(Core &core, std::uint32_t rootGuestAddress);

  const OtNode &node() const {
    return node_;
  }
  int nodesEntered() const {
    return nodesEntered_;
  }
  // False at the end of the chain and at the cap.
  bool advance();

private:
  Core &core_;
  OtNode node_;
  int nodesEntered_ = 0;
};

// The order a table's buckets are walked in: a reverse-cleared table (ClearOTagR) from its last
// bucket down, a forward-cleared one from its first up.
enum class OtWalk : std::uint8_t { HighToLow, LowToHigh };

// The ordering tables a title names, so the walk can say which bucket each packet was linked into.
// Several areas may share one id (a table per display buffer). A table whose bucket heads are nodes of
// the walked chain tags packets as the walk passes them; a title whose buckets are flattened into one
// chain before the walk assigns each packet its bucket instead.
class OtTables {
public:
  // `count` bucket heads `strideBytes` apart from `base`.
  void name(
      std::uint16_t table, std::uint32_t baseGuestAddress, std::uint32_t count, std::uint32_t strideBytes, OtWalk walk);
  // The packet at `packetGuestAddress` was linked into `slot`; holds until the next walk ends.
  void assign(std::uint32_t packetGuestAddress, present::OtSlot slot);
  void clear();
  // The bucket whose head node is at `nodeGuestAddress`.
  std::optional<present::OtSlot> slotOf(std::uint32_t nodeGuestAddress) const;
  // A walk begins: the assignments so far are what `assigned` answers from.
  void beginWalk();
  // The bucket the title last assigned the packet at `packetGuestAddress`.
  std::optional<present::OtSlot> assigned(std::uint32_t packetGuestAddress) const;
  // `slot` holds no packet this walk and is drawn right after the packet at `packetGuestAddress`.
  void placeEmpty(present::OtSlot slot, std::uint32_t packetGuestAddress);
  // The empty bucket drawn right after the packet at `packetGuestAddress`.
  std::optional<present::OtSlot> emptyAfter(std::uint32_t packetGuestAddress) const;
  // Whether `table` is walked from its high buckets down; true for a table never named.
  bool descending(std::uint16_t table) const;
  // A walk ended: assignments are spent.
  void endWalk();

private:
  struct Area {
    std::uint16_t table = 0;
    std::uint32_t base = 0; // main-RAM offset
    std::uint32_t count = 0;
    std::uint32_t stride = 4;
    OtWalk walk = OtWalk::HighToLow;
  };
  std::vector<Area> areas_;
  struct Assigned {
    std::uint32_t offset = 0; // packet main-RAM offset
    std::uint32_t order = 0;
    present::OtSlot slot;
  };
  std::vector<Assigned> assigned_; // by offset, then order, from beginWalk
  std::vector<Assigned> empties_;
  bool sorted_ = true;
};

// DMA2 linked-list mode: every node's GP0 words into the device, in link order.
void submitOrderingTable(Core &core, GpuDevice &device, std::uint32_t rootGuestAddress);

} // namespace psx::gpu
