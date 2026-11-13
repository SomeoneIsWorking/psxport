// segment_clock.h — how much of the segment in progress has already been committed to the clock.
#pragma once

#include <cstdint>

namespace psx::cpu {

// Lightrec charges a flat cycles_per_op of 2 per guest instruction; not an R3000 pipeline model (issue 0007).
inline constexpr std::uint32_t kLightrecCyclesPerInstruction = 2;

// The guest time a translated segment has already committed at device accesses, so an MMIO read
// mid-segment sees the clock move. Commits and the segment end count Lightrec's executed-instruction
// counter, not its cycle counter, which runs ahead of it, so a segment charges exactly its instructions
// however overrides split the code into segments.
class SegmentClockLedger {
public:
  // Start a new segment at Lightrec's executed-instruction count `executedInstructions`.
  void beginSegment(std::uint64_t executedInstructions) noexcept;

  // Instructions to commit at a device access that saw the executed-instruction count `executedInstructions`.
  std::uint32_t commitThrough(std::uint64_t executedInstructions) noexcept;

  // The segment's instructions less those already committed; saturates at zero.
  [[nodiscard]] std::uint64_t uncommitted(std::uint64_t segmentInstructions) const noexcept;

private:
  std::uint64_t segmentStart_ = 0;
  std::uint64_t committedInstructions_ = 0;
};

} // namespace psx::cpu
