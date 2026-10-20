// irq_edge.h — I_STAT is edge-latched: a held line sets its bit once and only the guest's ack clears
// it (beetle-psx irq.c IRQ_Assert).
#pragma once
#include <cstdint>

// Bit positions in I_STAT / I_MASK (0x1F801070 / 0x1F801074). Same numbering as beetle-psx irq.h.
enum IrqBit : std::uint8_t {
  IRQ_BIT_VBLANK = 0,
  IRQ_BIT_GPU = 1,
  IRQ_BIT_CD = 2,
  IRQ_BIT_DMA = 3,
  IRQ_BIT_TIMER0 = 4,
  IRQ_BIT_TIMER1 = 5,
  IRQ_BIT_TIMER2 = 6,
  IRQ_BIT_SIO = 7,
  IRQ_BIT_SPU = 9,
  IRQ_BIT_PIO = 10,
};

// The two interrupt registers themselves, as guest physical addresses.
inline constexpr uint32_t kIrqStatRegister = 0x1F801070u;
inline constexpr uint32_t kIrqMaskRegister = 0x1F801074u;

namespace psx::platform {

// Drives `bit`'s line in the caller's level word and returns the I_STAT bit a rising edge latches.
inline uint32_t latchIrqEdge(uint32_t &level, int bit, bool asserted) {
  const uint32_t was = level;
  const uint32_t mask = 1u << bit;
  level = asserted ? (level | mask) : (level & ~mask);
  return (was ^ level) & level;
}

} // namespace psx::platform
