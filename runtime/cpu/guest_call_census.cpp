#include "guest_call_census.h"

#include <lucent/log.h>

namespace psx::cpu {

void GuestCallCensus::recordCompleted(std::uint32_t entry,
                                      std::uint32_t returnPc,
                                      std::uint32_t turns,
                                      std::uint64_t cycles) {
  ++completed_;
  if (turns <= 1u) {
    return;
  }
  ++resumed_;
  resumedCycles_ += cycles;
  if (turns > deepestTurns_) {
    deepestTurns_ = turns;
  }
  lucent::info("guest-call",
               "guest call 0x{:08X} to return address 0x{:08X} outlived one host turn: {} turn(s), "
               "{} guest cycles. Denominator so far: {} of {} completed guest call(s) needed a resume; "
               "deepest {} turn(s)",
               entry,
               returnPc,
               turns,
               cycles,
               resumed_,
               completed_,
               deepestTurns_);
}

void GuestCallCensus::log(std::string_view why) const {
  if (completed_ == 0u) {
    lucent::info("guest-call", "run-end ({}): NO resumable guest call completed, so this run measured nothing", why);
    return;
  }
  if (resumed_ == 0u) {
    lucent::info("guest-call",
                 "run-end ({}): {} resumable guest call(s) completed, 0 needed a resume - every call "
                 "returned inside the one display field its host turn allows",
                 why,
                 completed_);
    return;
  }
  lucent::info("guest-call",
               "run-end ({}): {} resumable guest call(s) completed, {} needed a resume (deepest {} host "
               "turn(s), {} guest cycles over those calls); the other {} finished inside one field",
               why,
               completed_,
               resumed_,
               deepestTurns_,
               resumedCycles_,
               completed_ - resumed_);
}

} // namespace psx::cpu