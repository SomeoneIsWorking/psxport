// Accounting for the BIOS card syscalls a title actually makes.
//
// WHY THIS EXISTS. `card_hle_b0`/`card_hle_a0` dispatch by function number and end in
// `default: return 0` -- "not handled", silently, leaving no record that the call happened. A title
// that stalls waiting on a card function this runtime does not implement therefore produces no
// evidence at all: the card channel prints the image it opened and nothing else, and an
// investigator reads that silence as "the title made no card calls".
//
// MEASURED 2026-09-19 on Spyro 1: a run that reaches gameplay through the save menu and a run that
// stalls in title sub_state 12 for 12,000 fields emitted the SAME two card lines. The absence said
// nothing about either, because nothing in the dispatcher was counting.
//
// So this counts every dispatched function, handled and unhandled, and reports the totals with
// their denominator at teardown -- including the all-zero case, which is the one an investigator
// most needs to be able to tell apart from "the instrument never ran".
#pragma once

#include <cstdint>

namespace psxport::card {

// Which BIOS vector a call arrived on. The same function number means different things on each.
enum class Vector : uint8_t { A0, B0 };

class SyscallLog {
public:
  // Record one dispatch. `handled` is what the dispatcher is about to return: false means the
  // runtime has no implementation and the guest is about to be told the call was not taken.
  void record(Vector vector, uint32_t function, bool handled);

  // Emit the totals on the `card` channel, with the denominator. Always emits, including when
  // nothing was called at all -- that is a finding, not an absence.
  void report(const char *when) const;

  uint32_t calls() const {
    return mCalls;
  }
  uint32_t unhandled() const {
    return mUnhandled;
  }

private:
  // Function numbers are a byte, so a flat table per vector is exact and needs no map.
  static constexpr uint32_t kFunctions = 256;
  uint32_t mByFunction[2][kFunctions] = {};
  uint32_t mUnhandledByFunction[2][kFunctions] = {};
  uint32_t mCalls = 0;
  uint32_t mUnhandled = 0;
};

} // namespace psxport::card
