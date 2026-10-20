// present_ledger.cpp — per-producer recorded vs composed primitive counts of a t = 1 present.
#include "present_ledger.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <variant>

namespace psx::present {
namespace {

void collectProducers(const FrameRecord &record, std::vector<std::uint32_t> &producers) {
  producers.clear();
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive != nullptr && primitive->key) {
      producers.push_back(primitive->key->producer);
    }
  }
  std::ranges::sort(producers);
}

} // namespace

void PresentLedger::reconcile(const FrameRecord &shown, const FrameRecord &composed, std::uint64_t fence, bool fatal) {
  collectProducers(shown, recorded_);
  collectProducers(composed, presented_);
  bool dropped = false;
  for (auto run = recorded_.begin(); run != recorded_.end();) {
    const auto end = std::upper_bound(run, recorded_.end(), *run);
    if (!std::ranges::binary_search(presented_, *run)) {
      dropped = true;
      lucent::error("ledger",
                    "f{} producer {:#010x} recorded {} primitive(s) and the composed present drew none",
                    fence,
                    *run,
                    end - run);
    }
    run = end;
  }
  ++framesReconciled_;
  if (!dropped) {
    return;
  }
  ++framesDropped_;
  if (fatal) {
    std::abort();
  }
}

void PresentLedger::report() const {
  lucent::info("ledger",
               "run-end: {} composed t=1 present(s) reconciled, {} with a dropped producer",
               framesReconciled_,
               framesDropped_);
}

} // namespace psx::present
