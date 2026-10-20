// dma_irq.h — the DMA controller's registers and the DPCR (0x1F8010F0) / DICR (0x1F8010F4) rules
// (nocash psx-spx: DICR 15 force, 16-22 enable, 23 master enable, 24-30 flags, 31 computed master flag).
#pragma once
#include "guest_address_range.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::dma {

enum class DmaChannel : std::uint8_t {
  MdecIn = 0,
  MdecOut = 1,
  Gpu = 2,
  Cdrom = 3,
  Spu = 4,
  Pio = 5,
  Otc = 6,
};
inline constexpr std::size_t kChannelCount = 7;

constexpr std::uint32_t channelIndex(DmaChannel channel) {
  return static_cast<std::uint32_t>(channel);
}

// The BIOS leaves DPCR here and libraries read-modify-write it.
inline constexpr std::uint32_t kDpcrReset = 0x07654321u;

inline constexpr std::uint32_t kDicrForce = 0x00008000u;
inline constexpr std::uint32_t kDicrEnableMask = 0x007F0000u;
inline constexpr std::uint32_t kDicrMasterEnable = 0x00800000u;
inline constexpr std::uint32_t kDicrFlagMask = 0x7F000000u;
inline constexpr std::uint32_t kDicrMasterFlag = 0x80000000u;
// A store sets these directly; flag bits acknowledge on 1 and bit 31 is read-only.
inline constexpr std::uint32_t kDicrRwMask = 0x00FF803Fu;

// libstr read-modify-writes the byte at DICR+2, so stores are lane-masked.
inline std::uint32_t laneMask(GuestStore store) {
  if (store.width >= 4) {
    return 0xFFFFFFFFu;
  }
  const std::uint32_t shift = (store.address & 3u) * 8u;
  const std::uint32_t width = store.width * 8u;
  return ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << shift;
}

inline std::uint32_t laneValue(GuestStore store) {
  return (store.width >= 4) ? store.value : (store.value << ((store.address & 3u) * 8u));
}

inline std::uint32_t readDicr(std::uint32_t dicr) {
  const bool armedAndFlagged =
      (dicr & kDicrMasterEnable) != 0 && (dicr & ((dicr & kDicrEnableMask) << 8)) != 0;
  const std::uint32_t out = dicr & ~kDicrMasterFlag;
  return ((dicr & kDicrForce) || armedAndFlagged) ? (out | kDicrMasterFlag) : out;
}

inline std::uint32_t storeDicr(std::uint32_t current, GuestStore store) {
  const std::uint32_t lane = laneMask(store);
  const std::uint32_t value = laneValue(store) & lane;
  const std::uint32_t rw = kDicrRwMask & lane;
  const std::uint32_t ack = kDicrFlagMask & lane & value;
  return ((current & ~rw) | (value & rw)) & ~ack;
}

inline bool irqArmed(std::uint32_t dicr, DmaChannel channel) {
  return (dicr & kDicrMasterEnable) != 0 && (dicr & (1u << (16u + channelIndex(channel)))) != 0;
}

inline std::uint32_t completeDicr(std::uint32_t dicr, DmaChannel channel) {
  return irqArmed(dicr, channel) ? (dicr | (1u << (24u + channelIndex(channel)))) : dicr;
}

inline std::uint32_t ackDicr(std::uint32_t dicr, DmaChannel channel) {
  return dicr & ~(1u << (24u + channelIndex(channel)));
}

// Channel `ch`'s entry in the BIOS DMACallback table at `table`; a zero table has none.
inline std::uint32_t callbackSlot(std::uint32_t table, DmaChannel channel) {
  return table ? table + 4u * channelIndex(channel) : 0u;
}

// Channels whose armed completion still owes the guest its callback, taken at a function-entry boundary.
struct DmaDone {
  std::uint32_t mask = 0;

  bool complete(std::uint32_t &dicr, DmaChannel channel) {
    if (!irqArmed(dicr, channel)) {
      return false;
    }
    dicr = completeDicr(dicr, channel);
    mask |= 1u << channelIndex(channel);
    return true;
  }
  bool owed(DmaChannel channel) const {
    return (mask & (1u << channelIndex(channel))) != 0;
  }
  void taken(DmaChannel channel) {
    mask &= ~(1u << channelIndex(channel));
  }
  bool any() const {
    return mask != 0;
  }
};

struct ChannelRegisters {
  std::uint32_t madr = 0;
  std::uint32_t bcr = 0;
  std::uint32_t chcr = 0;
};

// Every DMA register this runtime models, per Core.
struct DmaRegisters {
  std::array<ChannelRegisters, kChannelCount> channel{};
  // DMA0/DMA1 pending cursors around the MDEC (next word, words left).
  std::uint32_t mdec0Addr = 0;
  std::int32_t mdec0Left = 0;
  std::uint32_t mdec1Addr = 0;
  std::int32_t mdec1Left = 0;
  // Last SPU transfer-start address (0x1F801DA6 << 3).
  std::uint32_t spuXferAddr = 0;
  std::uint32_t dpcr = kDpcrReset;
  std::uint32_t dicr = 0;
  DmaDone done;

  ChannelRegisters &of(DmaChannel id) {
    return channel[channelIndex(id)];
  }
  const ChannelRegisters &of(DmaChannel id) const {
    return channel[channelIndex(id)];
  }
  // The port runs the callback in place of the BIOS DMA handler, which acknowledges the flag first.
  void acknowledge(DmaChannel id) {
    done.taken(id);
    dicr = ackDicr(dicr, id);
  }
};

} // namespace psx::dma
