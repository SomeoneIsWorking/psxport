// segment_clock.h — how much of the segment in progress has already been committed to the clock.
#pragma once

#include <cstdint>

namespace psx::cpu {

// Lightrec charges a flat cycles_per_op of 2 per guest instruction; not an R3000 pipeline model (issue 0007).
inline constexpr std::uint32_t kLightrecCyclesPerInstruction = 2;

// The guest time a translated segment has already committed at device accesses, so an MMIO read
// mid-segment sees the clock move. Every commit is a delta within one segment and never negative.
class SegmentClockLedger {
public:
  // Start a new segment; the caller resets Lightrec's cycle counter with it.
  void beginSegment() noexcept;

  // Instructions to commit at a device access that saw `currentCycle`; 0 when the counter did not move
  // or moved backwards (a nested execution reset it), which re-anchors the baseline.
  std::uint32_t commitThrough(std::uint32_t currentCycle) noexcept;

  // The segment's instructions less those already committed; saturates at zero.
  [[nodiscard]] std::uint64_t uncommitted(std::uint64_t segmentInstructions) const noexcept;

private:
  // Last `current_cycle` already converted.
  std::uint32_t chargedThroughCycles_ = 0;
  // Cycles short of a whole instruction, carried to the next delta.
  std::uint32_t pendingCycles_ = 0;
  std::uint64_t committedInstructions_ = 0;
};

} // namespace psx::cpu
