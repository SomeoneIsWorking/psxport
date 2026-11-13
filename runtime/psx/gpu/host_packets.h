// host_packets.h — GP0 packets a title draws inside a guest DrawOTag walk without storing them in guest RAM.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

class Core;

namespace psx::gpu {

class GpuDevice;

// Each packet is drawn just before a named node of the walk from a named root, as if the guest had linked it
// there. Storage is fixed: running out is a named fault, never a dropped packet.
class HostPackets {
public:
  static constexpr std::size_t kPacketCapacity = 4096;
  static constexpr std::size_t kWordCapacity = kPacketCapacity * 4;

  HostPackets();

  // `words` are drawn right before `nodeGuestAddress` when the walk from `rootGuestAddress` enters it, after any
  // packet inserted there earlier.
  void insertBefore(Core &core,
                    std::uint32_t rootGuestAddress,
                    std::uint32_t nodeGuestAddress,
                    std::span<const std::uint32_t> words);
  // The table at `rootGuestAddress` is being rebuilt; what was inserted into it is gone.
  void discard(std::uint32_t rootGuestAddress);

  // The walk from `rootGuestAddress` begins; true when it has packets to draw.
  bool beginWalk(std::uint32_t rootGuestAddress);
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
    bool drawn = false;
  };
  std::vector<Packet> packets_;
  std::vector<std::uint32_t> words_;
  // The walk's packets by node, then insertion order.
  std::vector<std::uint32_t> walkOrder_;
  std::uint32_t walkRoot_ = 0;
  std::size_t walkPending_ = 0;
  bool walking_ = false;
};

} // namespace psx::gpu
