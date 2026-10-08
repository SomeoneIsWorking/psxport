// ordering_table.h — the guest's DrawOTag linked list, read as named things.
//
// A PSX game submits its frame by walking a linked list of 32-bit NODES in main RAM. Each node is a
// header word followed by that primitive's GP0 command stream, and the header word packs two unrelated
// fields into one 32-bit value. Reading it inline is what made this list opaque: `hdr >> 24` and
// `hdr & 0xFFFFFF` appeared at all four places the runtime touched a node, and a reader had to remember
// which half meant which.
//
// This owner names both fields, owns the one RAM read, and owns the two end-of-chain sentinels, so the
// OT walk, the malformed-list probe and the per-node diagnostics cannot each re-derive the layout.
#ifndef PSXPORT_ORDERING_TABLE_H
#define PSXPORT_ORDERING_TABLE_H

#include "frame_record.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

class Core; // runtime/psx/core/core.h — the guest RAM handle the walk reads through

namespace psx::gpu {

class GpuDevice;

// The next-address FIELD of a node header, and the value that ends the chain.
//
// The field is 24 bits wide, so its all-ones value doubles as the hardware's "last node" sentinel — one
// constant doing two jobs is not a coincidence here: the sentinel IS "every address bit set". The field
// is then narrowed to an actual main-RAM offset by kMainRamOffsetMask, which is what clears the two
// low bits: a guest's next-node pointer is a word address, so those bits are always zero and a set bit
// is a malformed node rather than a different address.
inline constexpr std::uint32_t kOtNextAddressMask = 0xFFFFFFu;
inline constexpr std::uint32_t kOtChainEnd = kOtNextAddressMask;
// A plain zero ALSO ends the chain. Retail toolchains emit that form, and treating it as an address is
// how a malformed table turns into a walk through the whole 2 MB window and past it.
inline constexpr std::uint32_t kOtChainEndZero = 0;
// A node address is the low 24 bits of a guest address: the 2 MB main-RAM window, four-byte aligned.
inline constexpr std::uint32_t kMainRamOffsetMask = 0x1FFFFCu;
// The KSEG0 alias the GPU — and every diagnostic that names a node — addresses main RAM through.
inline constexpr std::uint32_t kKseg0Base = 0x80000000u;
// Bytes a node's header word occupies before its GP0 stream starts. Named because "4" here means
// "one word of header", not "four bytes of anything".
inline constexpr std::uint32_t kOtHeaderBytes = 4;
// The walk's own node cap. A DrawOTag is bounded by the packet pool (tens of thousands of nodes at the
// very top end), so exceeding this means the chain is cyclic or malformed. It is a REFUSAL to keep
// walking, not a guess about how long the table is.
inline constexpr int kOtNodeLimit = 0x10000;

// The guest address of a main-RAM offset, as the guest wrote it. Reported rather than a bare offset so
// a diagnostic can name a node that guest code can go and look at.
constexpr std::uint32_t guestAddressOf(std::uint32_t mainRamOffset) {
  return kKseg0Base | (mainRamOffset & kMainRamOffsetMask);
}
// A guest address reduced to the main-RAM offset a node header actually holds.
constexpr std::uint32_t mainRamOffsetOf(std::uint32_t guestAddress) {
  return guestAddress & kMainRamOffsetMask;
}

// ONE node of the chain, decoded.
//
//     31                    24 23                     0
//    +------------------------+------------------------+
//    | GP0 word count (0-255)  | next node's RAM offset |
//    +------------------------+------------------------+
//
// A count of 0 is a LINK-ONLY node: it holds no primitive and exists only to keep the chain going.
// That is not a degenerate case — an emptied table is exactly that, so "0 words" has to mean "walk on"
// and not "stop".
class OtNode {
public:
  constexpr OtNode() = default;
  constexpr OtNode(std::uint32_t mainRamOffset, std::uint32_t headerWord)
      : ramOffset_(mainRamOffset), header_(headerWord) {}

  // This node's own main-RAM offset.
  constexpr std::uint32_t ramOffset() const {
    return ramOffset_;
  }
  constexpr std::uint32_t guestAddress() const {
    return guestAddressOf(ramOffset_);
  }
  // The raw header word, kept for a diagnostic that prints it verbatim.
  constexpr std::uint32_t headerWord() const {
    return header_;
  }

  // How many GP0 command words this node's primitive occupies.
  constexpr unsigned gp0WordCount() const {
    return header_ >> 24;
  }
  // Whether this node carries a primitive at all (a link-only node does not).
  constexpr bool carriesPrimitive() const {
    return gp0WordCount() != 0;
  }

  // GP0 command word `index` of this node's stream, as a main-RAM offset. Word 0 is the command word
  // itself and sits immediately after the header.
  constexpr std::uint32_t gp0WordRamOffset(unsigned index) const {
    return ramOffset_ + kOtHeaderBytes + kOtHeaderBytes * index;
  }

  // The next-address field exactly as the guest wrote it, BEFORE it is narrowed to a real offset. Both
  // end-of-chain sentinels are visible here, which is why `endsChain` tests the field and not the
  // narrowed value: 0xFFFFFF narrows to 0x1FFFFC, a plausible address, so testing after the mask would
  // miss the sentinel this owner exists to honour.
  constexpr std::uint32_t nextAddressField() const {
    return header_ & kOtNextAddressMask;
  }
  // The next node's main-RAM offset, or one of the two end-of-chain sentinels.
  constexpr std::uint32_t nextRamOffset() const {
    return nextAddressField() & kMainRamOffsetMask;
  }
  // Whether the chain stops here. BOTH sentinels count: see kOtChainEnd / kOtChainEndZero.
  constexpr bool endsChain() const {
    const std::uint32_t next = nextAddressField();
    return next == kOtChainEnd || next == kOtChainEndZero;
  }
  constexpr std::uint32_t nextGuestAddress() const {
    return guestAddressOf(nextRamOffset());
  }

private:
  std::uint32_t ramOffset_ = 0;
  std::uint32_t header_ = 0;
};

// A CURSOR over one guest DrawOTag. It is the only thing in the runtime that knows how a node's header
// word splits, so the walk, the malformed-list probe and the per-node diagnostics cannot drift apart.
//
// The cursor refuses a cyclic or malformed list rather than following it: `advance()` stops at the
// chain's own end AND at kOtNodeLimit, and `truncated()` says which of the two happened. That
// distinction is the point — "the table ended" and "the walk gave up" are opposite answers, and a walk
// that reports one while meaning the other turns a malformed table into a frame that never presents.
class OrderingTableCursor {
public:
  explicit OrderingTableCursor(Core &core, std::uint32_t rootGuestAddress);

  // The node the cursor is on. Valid from construction onwards.
  const OtNode &node() const {
    return node_;
  }
  // How many nodes this walk has actually read. The denominator a "N nodes" line needs.
  int nodesEntered() const {
    return nodesEntered_;
  }
  // Whether the walk stopped because the chain did not terminate inside kOtNodeLimit nodes.
  bool truncated() const {
    return truncated_;
  }

  // Move to the next node. Returns false at the end of the chain and at the cap.
  bool advance();

private:
  Core &core_;
  OtNode node_;
  int nodesEntered_ = 0;
  bool truncated_ = false;
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
  // The bucket the title assigned the packet at `packetGuestAddress`.
  std::optional<present::OtSlot> assigned(std::uint32_t packetGuestAddress) const;
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
  std::unordered_map<std::uint32_t, present::OtSlot> assigned_; // by packet main-RAM offset
};

// DMA2 linked-list mode: every node's GP0 words into the device, in link order.
void submitOrderingTable(Core &core, GpuDevice &device, std::uint32_t rootGuestAddress);

} // namespace psx::gpu

#endif // PSXPORT_ORDERING_TABLE_H
