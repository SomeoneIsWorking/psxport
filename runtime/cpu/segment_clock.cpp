#include "segment_clock.h"

namespace psx::cpu {

void SegmentClockLedger::beginSegment() noexcept {
  chargedThroughCycles_ = 0;
  pendingCycles_ = 0;
  committedInstructions_ = 0;
}

std::uint32_t SegmentClockLedger::commitThrough(std::uint32_t currentCycle) noexcept {
  if (currentCycle <= chargedThroughCycles_) {
    chargedThroughCycles_ = currentCycle;
    return 0;
  }
  const std::uint64_t cycles = static_cast<std::uint64_t>(currentCycle - chargedThroughCycles_) + pendingCycles_;
  chargedThroughCycles_ = currentCycle;
  const auto instructions = static_cast<std::uint32_t>(cycles / kLightrecCyclesPerInstruction);
  pendingCycles_ = static_cast<std::uint32_t>(cycles % kLightrecCyclesPerInstruction);
  committedInstructions_ += instructions;
  return instructions;
}

std::uint64_t SegmentClockLedger::uncommitted(std::uint64_t segmentInstructions) const noexcept {
  return segmentInstructions > committedInstructions_ ? segmentInstructions - committedInstructions_ : 0;
}

} // namespace psx::cpu
