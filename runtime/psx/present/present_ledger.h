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
  // The producers `composed` dropped relative to `shown`, each logged; `fatal` aborts on a drop.
  std::vector<std::uint32_t>
  reconcile(const FrameRecord &shown, const FrameRecord &composed, std::uint64_t fence, bool fatal);
  // The run's totals, with the denominator, so a run that composed nothing cannot read as clean.
  void report() const;

  std::uint64_t framesReconciled() const {
    return framesReconciled_;
  }
  std::uint64_t framesDropped() const {
    return framesDropped_;
  }

private:
  std::uint64_t framesReconciled_ = 0;
  std::uint64_t framesDropped_ = 0;
};

} // namespace psx::present
