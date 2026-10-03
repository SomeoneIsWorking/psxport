// guest_call_census — the run's tally of resumable guest calls, per Core.
//
// Every call that crossed a host turn passes through `ResumableGuestCall`, so the counters live
// there rather than in every title that owns one. A run that resumed nothing must still SAY so:
// without a denominator, silence and "no call was ever long enough" look identical.
#pragma once

#include <cstdint>
#include <string_view>

namespace psx::cpu {

class GuestCallCensus {
public:
  // One completed call, with the host turns and guest cycles it spent. `entry`/`returnPc` name the
  // call in the per-call line; a call that returned inside its first turn is counted and not logged.
  void recordCompleted(std::uint32_t entry, std::uint32_t returnPc, std::uint32_t turns, std::uint64_t cycles);

  [[nodiscard]] std::uint64_t completed() const {
    return completed_;
  }
  [[nodiscard]] std::uint64_t resumed() const {
    return resumed_;
  }
  [[nodiscard]] std::uint32_t deepestTurns() const {
    return deepestTurns_;
  }
  [[nodiscard]] std::uint64_t resumedCycles() const {
    return resumedCycles_;
  }

  // The run-end line, with its denominators. `why` names why the run ended.
  void log(std::string_view why) const;

private:
  std::uint64_t completed_ = 0;
  std::uint64_t resumed_ = 0;
  std::uint64_t resumedCycles_ = 0;
  std::uint32_t deepestTurns_ = 0;
};

} // namespace psx::cpu