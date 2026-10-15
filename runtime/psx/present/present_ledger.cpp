// present_ledger.cpp — per-producer recorded vs composed primitive counts of a t = 1 present.
#include "present_ledger.h"

#include <lucent/log.h>

#include <cstdlib>
#include <map>
#include <variant>

namespace psx::present {
namespace {

std::map<std::uint32_t, long> keyedPrimitives(const FrameRecord &record) {
  std::map<std::uint32_t, long> counts;
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive != nullptr && primitive->key) {
      counts[primitive->key->producer]++;
    }
  }
  return counts;
}

} // namespace

std::vector<std::uint32_t>
PresentLedger::reconcile(const FrameRecord &shown, const FrameRecord &composed, std::uint64_t fence, bool fatal) {
  const std::map<std::uint32_t, long> recorded = keyedPrimitives(shown);
  const std::map<std::uint32_t, long> presented = keyedPrimitives(composed);
  std::vector<std::uint32_t> dropped;
  for (const auto &[producer, count] : recorded) {
    if (!presented.contains(producer)) {
      dropped.push_back(producer);
      lucent::error("ledger",
                    "f{} producer {:#010x} recorded {} primitive(s) and the composed present drew none",
                    fence,
                    producer,
                    count);
    }
  }
  ++framesReconciled_;
  if (dropped.empty()) {
    return dropped;
  }
  ++framesDropped_;
  if (fatal) {
    std::abort();
  }
  return dropped;
}

void PresentLedger::report() const {
  lucent::info("ledger",
               "run-end: {} composed t=1 present(s) reconciled, {} with a dropped producer",
               framesReconciled_,
               framesDropped_);
}

} // namespace psx::present
