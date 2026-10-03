// segment_clock.h — how much of the segment in progress has already been committed to the clock.
#pragma once

#include <cstdint>

namespace psx::cpu {

// Lightrec charges a FLAT cost per guest instruction, not an R3000 pipeline: `state->cycles_per_op`
// is set to 2 in `lightrec.c` and every emitter charges exactly that much per opcode (and per
// modelled delay slot), with no per-class table. This is the divisor that turns Lightrec's own
// cycle budget into the instruction unit this clock is expressed in, so the two counters describe
// the same guest work and can never disagree about how much of it happened.
//
// It is an APPROXIMATION of the hardware and deliberately not more than one. A real R3000A spends a
// different number of cycles per instruction class (loads and branches differ from ALU ops, and
// mul/div are tens of cycles), so neither this constant nor `EmulatedTime` is a cycle-accurate CPU
// model — `runtime/psx/frame/timing.h` records that same gap and cites issue 0007. What this constant does
// guarantee is INTERNAL CONSISTENCY: mid-segment commits and the segment's own instruction total are
// two views of one measurement, so a commit never invents or loses guest time relative to the total
// the executor was going to charge anyway. The end state that removes the approximation is issue
// 0007's cycle-accurate R3000 model, at which point the emulated clock and Lightrec's budget are
// the same quantity and this constant disappears with it.
inline constexpr std::uint32_t kLightrecCyclesPerInstruction = 2;

// The per-segment commitment ledger for the emulated guest clock.
//
// WHY IT EXISTS. The executor accounts a segment's guest instructions once, after `lightrec_execute`
// returns (`LightrecExecutor::executeWithBoundary`). `EmulatedTime` moves only there, so a guest
// polling a hardware counter from inside one translated segment — the `latch RCnt2, spin until the
// delta exceeds N` idiom — read the SAME value for the whole segment and could not leave its loop
// until the segment ended. Measured on Tekken 3 (SLUS_004.02) `FUN_80093478`: identical guest bytes
// exhausted the whole budget at `0x80093584` in one 564,492-cycle segment and left the loop at
// `0x800934D8` in nine shorter ones. The contract this restores is `psxport/AGENTS.md`'s — all
// guest-visible state and elapsed cycles are committed BEFORE a device callback is observed by host
// code — and an MMIO register read IS a device callback.
//
// THE DELTA, and why it cannot run backwards. `lightrec_current_cycle_count` returns
// `state->current_cycle`, which the RW wrapper writes as `target_cycle - LIGHTREC_REG_CYCLE` on
// entry, i.e. the live cycles consumed so far in the segment. It is monotonically non-decreasing
// WITHIN a segment, and `LightrecExecutor` resets it to 0 (`lightrec_reset_cycle_count`) at every
// segment start — so it is NOT monotonic across one. Every commit therefore charges a DELTA against
// a baseline taken from the same counter in the same segment, and `beginSegment` resets the baseline
// in the same breath as the counter. An absolute read would subtract a small number from a counter
// that had just restarted and walk the clock backwards at every boundary; that is a second defect
// layered on the first, and this class is where it is prevented.
//
// Monotonicity of the clock is a property of the code and not of any caller: every path out of
// `commitThrough` returns a count of instructions to ADD, and `uncommitted` saturates at zero. A
// caller cannot make the clock run backwards, whatever the counter does.
class SegmentClockLedger {
public:
  // Start a new segment. The caller MUST have reset Lightrec's cycle counter in the same breath:
  // the baseline and the counter are two views of one sequence and are meaningless apart.
  void beginSegment() noexcept;

  // Instructions to commit to the clock now, for a device access that observed `currentCycle` as
  // Lightrec's cycle count. Returns 0 when the counter has not moved since the last commit, and also
  // when it moved BACKWARDS — a nested guest execution resets the counter underneath us and the
  // outer segment's cycle count is then unknowable, so the baseline is re-anchored and nothing is
  // charged. The outer segment's own accounting still charges its full instruction total, so no
  // guest time is lost by declining.
  std::uint32_t commitThrough(std::uint32_t currentCycle) noexcept;

  // What the segment's own accounting still owes the clock: its executed instruction count less
  // what has already been committed here. Saturates at zero, so over-committing a segment (a
  // conversion that rounds up past the true count) can never subtract from the clock.
  [[nodiscard]] std::uint64_t uncommitted(std::uint64_t segmentInstructions) const noexcept;

  // Instructions committed inside the segment so far. This is the denominator that separates
  // "no device access charged anything" from "no device access happened".
  [[nodiscard]] std::uint64_t committedInstructions() const noexcept {
    return committedInstructions_;
  }
  [[nodiscard]] std::uint32_t chargedThroughCycles() const noexcept {
    return chargedThroughCycles_;
  }

private:
  // Last `current_cycle` value already converted. Zero at every segment start, beside the counter.
  std::uint32_t chargedThroughCycles_ = 0;
  // Cycles converted so far that do not add up to a whole instruction, carried so that a stream of
  // small deltas loses nothing. `kLightrecCyclesPerInstruction` divides every delta the JIT charges
  // in practice, so this is normally zero — carried so the property does not depend on that.
  std::uint32_t pendingCycles_ = 0;
  std::uint64_t committedInstructions_ = 0;
};

} // namespace psx::cpu
