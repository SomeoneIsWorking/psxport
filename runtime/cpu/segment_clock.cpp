#include "segment_clock.h"

namespace psx::cpu {

void SegmentClockLedger::beginSegment(std::uint64_t executedInstructions) noexcept {
  segmentStart_ = executedInstructions;
  committedInstructions_ = 0;
}

std::uint32_t SegmentClockLedger::commitThrough(std::uint64_t executedInstructions) noexcept {
  const std::uint64_t executed = executedInstructions > segmentStart_ ? executedInstructions - segmentStart_ : 0;
  if (executed <= committedInstructions_) {
    return 0;
  }
  // A segment's budget is bounded by Lightrec's 32-bit cycle target, so a delta fits.
  const auto instructions = static_cast<std::uint32_t>(executed - committedInstructions_);
  committedInstructions_ = executed;
  return instructions;
}

std::uint64_t SegmentClockLedger::uncommitted(std::uint64_t segmentInstructions) const noexcept {
  return segmentInstructions > committedInstructions_ ? segmentInstructions - committedInstructions_ : 0;
}

} // namespace psx::cpu
