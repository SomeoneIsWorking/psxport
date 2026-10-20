// present_ledger.h — producers whose recorded primitives a composed t = 1 present drew for nobody.
#pragma once

#include "frame_record.h"

#include <cstdint>
#include <vector>

namespace psx::present {

// At t = 1 every object the shown record drew has its state in that record, so a producer whose
// entries the composer removed must draw something in their place. One that draws nothing is dropped.
class PresentLedger {
public:
  // Logs each producer `composed` dropped relative to `shown`; `fatal` aborts on a drop.
  void reconcile(const FrameRecord &shown, const FrameRecord &composed, std::uint64_t fence, bool fatal);
  // The run's totals, with the denominator, so a run that composed nothing cannot read as clean.
  void report() const;

private:
  std::uint64_t framesReconciled_ = 0;
  std::uint64_t framesDropped_ = 0;
  // Sorted producer keys of each keyed primitive, reused across frames.
  std::vector<std::uint32_t> recorded_;
  std::vector<std::uint32_t> presented_;
};

} // namespace psx::present
