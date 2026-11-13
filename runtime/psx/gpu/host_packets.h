// host_packets.h — GP0 packets a title draws inside a guest DrawOTag walk without storing them in guest RAM.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Core;

namespace psx::platform {
class BlobReader;
class BlobWriter;
} // namespace psx::platform

namespace psx::gpu {

class GpuDevice;

// Each packet is drawn just before a named node of the walk from a named root, as if the guest had linked it
// there; a named node's own packet can be drawn as other words and moved. Storage is fixed: running out is a named
// fault, never a dropped packet or move. What a walk drew stays readable until the next walk begins.
class HostPackets {
public:
  static constexpr std::size_t kPacketCapacity = 4096;
  static constexpr std::size_t kWordCapacity = kPacketCapacity * 4;
  static constexpr std::size_t kMoveCapacity = 8192;

  HostPackets();

  // `words` are drawn right before `nodeGuestAddress` when the walk from `rootGuestAddress` enters it, after any
  // packet inserted there earlier.
  void insertBefore(Core &core,
                    std::uint32_t rootGuestAddress,
                    std::uint32_t nodeGuestAddress,
                    std::span<const std::uint32_t> words);
  // The walk from `rootGuestAddress` draws the packet of `nodeGuestAddress` with every vertex `dx` columns right;
  // guest RAM keeps what the guest wrote. Moves of one node add up.
  void moveX(Core &core, std::uint32_t rootGuestAddress, std::uint32_t nodeGuestAddress, int dx);
  // The walk from `rootGuestAddress` draws `words`, as many as the node's packet holds, in place of the packet of
  // `nodeGuestAddress`; its moves apply to them and guest RAM keeps what the guest wrote. A later replacement of
  // the node replaces the earlier.
  void replace(Core &core,
               std::uint32_t rootGuestAddress,
               std::uint32_t nodeGuestAddress,
               std::span<const std::uint32_t> words);
  // The table at `rootGuestAddress` is being rebuilt; what was inserted into or moved in it is gone.
  void discard(std::uint32_t rootGuestAddress);

  // What tables built but not yet walked hold, for the machine state: the guest draw that made it has run and no
  // guest byte records it.
  void save(platform::BlobWriter &out) const;
  // False with `error` set and nothing changed when the stream does not fit the fixed storage.
  bool load(platform::BlobReader &in, std::string &error);

  // The table later lookups answer for (OrderingTableWalk).
  void select(std::uint32_t rootGuestAddress);
  // `visit(words)` for each packet drawn right before `nodeGuestAddress` in the selected table, in order.
  template <typename Visit> void forEachBefore(std::uint32_t nodeGuestAddress, Visit visit) const {
    const std::uint32_t node = ramOffset(nodeGuestAddress);
    for (auto at = firstBefore(nodeGuestAddress); at != order_.end() && packets_[*at].node == node; ++at) {
      const Packet &packet = packets_[*at];
      visit(std::span<const std::uint32_t>(words_.data() + packet.firstWord, packet.wordCount));
    }
  }
  // The columns the selected table's walk moves `nodeGuestAddress`'s own packet, 0 when none.
  [[nodiscard]] int movedX(std::uint32_t nodeGuestAddress) const;
  // The words the selected table's walk draws for `nodeGuestAddress`'s own packet, empty when the guest's.
  [[nodiscard]] std::span<const std::uint32_t> replaced(std::uint32_t nodeGuestAddress) const;
  [[nodiscard]] bool selectedEmpty() const {
    return order_.empty() && orderMoves_.empty() && orderReplaced_.empty();
  }

  // The DrawOTag walk from `rootGuestAddress` begins: what the last walk drew is dropped and this table selected.
  void beginWalk(std::uint32_t rootGuestAddress);
  // The walk is about to enter `nodeGuestAddress`: its packets into `device`.
  void drawBefore(GpuDevice &device, std::uint32_t nodeGuestAddress);
  // The walk ended. A packet whose node it never entered is a named fault: its title linked it to a node off
  // the chain.
  void endWalk(Core &core);

private:
  struct Packet {
    std::uint32_t root = 0; // main-RAM offsets
    std::uint32_t node = 0;
    std::uint32_t firstWord = 0;
    std::uint32_t wordCount = 0;
    bool walked = false;
  };
  struct Move {
    std::uint32_t root = 0; // main-RAM offsets
    std::uint32_t node = 0;
    int dx = 0;
    bool walked = false;
  };
  static std::uint32_t ramOffset(std::uint32_t guestAddress);
  static void writePackets(platform::BlobWriter &out, std::span<const Packet> packets);
  // False when the stream holds more than kPacketCapacity.
  static bool readPackets(platform::BlobReader &in, std::vector<Packet> &packets);
  std::vector<std::uint32_t>::const_iterator firstBefore(std::uint32_t nodeGuestAddress) const;
  [[noreturn]] void refuse(Core &core, std::string_view what, std::uint32_t root, std::uint32_t node) const;
  // Drops the packets and moves `drop` names, keeping the stored words of the rest in order.
  template <typename Drop> void erase(Drop drop);

  std::vector<Packet> packets_;
  std::vector<Packet> replacements_; // a packet drawn in place of its node's own
  std::vector<std::uint32_t> words_;
  std::vector<Move> moves_;
  // The selected table's packets by node, then insertion order, its moves by node, and its replacements by node,
  // the last of a node first.
  std::vector<std::uint32_t> order_;
  std::vector<Move> orderMoves_;
  std::vector<std::uint32_t> orderReplaced_;
  std::uint32_t selectedRoot_ = 0;
  bool selected_ = false;
  std::size_t walkPending_ = 0;
  bool walking_ = false;
};

} // namespace psx::gpu
