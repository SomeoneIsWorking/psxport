// irq_edge.h — how a hardware source's interrupt LINE becomes a bit in I_STAT.
//
// This is the one rule that is easy to get wrong in both directions, so it lives on its own and is
// pinned by tests/test_spu_irq_line.cpp: I_STAT is EDGE-latched, not level-driven. A source that holds
// its line asserted sets the bit ONCE; the guest acknowledges by writing a 0 to that bit of I_STAT
// (mem.cpp), and the source must go low and high again before the bit comes back. Driving I_STAT from
// the level instead would both re-assert immediately after every ack and drop a still-pending
// interrupt the moment the source deasserted.
//
// Transcribed from the semantics of vendor/beetle-psx/mednafen/psx/irq.c IRQ_Assert, which is the
// reference this framework's SPU line goes through (spu.c calls IRQ_Assert; runtime/psx/boot/hw_bind.cpp
// turns that into Game::hle.i_stat bit 9).
#pragma once
#include <cstdint>

// Bit positions in I_STAT / I_MASK (0x1F801070 / 0x1F801074). Same numbering as beetle-psx irq.h.
enum : std::uint8_t {
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

// Drive source `bit`'s line to `asserted`, updating the caller's LEVEL word in place, and return the
// I_STAT bit to latch: set only on that source's low->high transition. The caller ORs it in; only
// the guest's ack ever clears an I_STAT bit.
inline uint32_t latchIrqEdge(uint32_t &level, int bit, bool asserted) {
  const uint32_t was = level;
  const uint32_t mask = 1u << bit;
  level = asserted ? (level | mask) : (level & ~mask);
  return (was ^ level) & level;
}

} // namespace psx::platform
