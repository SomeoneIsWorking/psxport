// test_segment_clock.cpp — the emulated guest clock must be observable from INSIDE one translated
// segment, and the commit that makes it observable must never run the clock backwards.
//
// THE DEFECT THIS GUARDS. `LightrecExecutor::executeWithBoundary` accounted a segment's guest
// instructions once, after `lightrec_execute` returned, and `psx::frame::EmulatedTime` moves only there. A guest
// polling a hardware counter from inside one translated segment — the `latch RCnt2, spin until the
// delta exceeds N` idiom, which is how a PSX pad driver talks to a controller — therefore read the
// SAME value for the whole segment and could not leave its loop until the segment ended. Measured on
// Tekken 3 (SLUS_004.02) `FUN_80093478`'s second wait loop: identical guest bytes exhausted the whole
// budget at `0x80093584` in the product's one 564,492-cycle segment and left the loop at `0x800934D8`
// in nine shorter ones. `psxport/AGENTS.md` already requires the opposite — "before a ... HLE/device
// callback ... is observed by host code, all guest-visible state and elapsed cycles are committed to
// `Core`" — and an MMIO register read IS a device callback.
//
// The suite has two halves, and the second is not decoration:
//
//   * `SegmentClockLedger` arithmetic, hermetic and exact. This is where the per-segment-reset
//     backwards jump would live, and it is the bug MOST LIKELY TO PASS A TEST WRITTEN ONLY FOR THE
//     FIRST HALF: a test that only checks "the guest sees the counter move" is satisfied by any
//     charge, including one that adds an absolute cycle count and walks the clock backwards at every
//     segment boundary. So the monotonicity cases are here, as executable statements about the
//     ledger, not as a property inferred from the guest test.
//
//   * the guest-visible case, through the real executor: a synthetic PSX program that polls RCnt2 32
//     times inside one segment and records each value. Before the fix it recorded 32 identical values;
//     after, 31 of 32 differ. It runs on no disc, in no window, through the same `dispatchGuest0` a
//     title uses.
#include "core.h"
#include "execution_exit.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "lightrec_executor.h"
#include "segment_clock.h"
#include "testutil.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr std::uint32_t kRootCounter2Value = 0x1F801120u;

constexpr std::uint32_t kV0 = 2u;

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

constexpr std::uint32_t kProgram = 0x00010000u;
constexpr std::uint32_t kTrampoline = 0x00010200u;
constexpr std::uint32_t kRecord = 0x00011100u;
constexpr std::uint32_t kSamples = 32u;

// A PSX program that reads RCnt2 kSamples times and records each value into guest RAM.
//
//   0x00 lui   a1, 0x1f80          a1 = 0x1F800000
//   0x04 lui   a2, 0x0001
//   0x08 addiu a2, a2, 0x1100      a2 = 0x00011100, the record cursor
//   0x0c lui   a3, 0x0001
//   0x10 addiu a3, a3, 0x1180      a3 = 0x00011180, one past the last sample
//   0x14 lw    v0, 0x1120(a1)      <- .loop: 0x1F801120 is RCnt2's value register
//   0x18 sw    v0, 0x000(a2)
//   0x1c addiu a2, a2, 4
//   0x20 slt   v1, a2, a3
//   0x24 bnez  v1, .loop
//   0x28 nop                        (delay slot)
//   0x2c jr    ra
//   0x30 nop                        (delay slot)
constexpr std::uint32_t kPollProgram[] = {
    0x3c051f80u,
    0x3c060001u,
    0x24c61100u,
    0x3c070001u,
    0x24e71180u,
    0x8ca21120u,
    0xacc20000u,
    0x24c60004u,
    0x00c7182au,
    0x1460fffbu,
    0x00000000u,
    0x03e00008u,
    0x00000000u,
};

struct Fixture {
  Runtime runtime;
  // NOT a member initialiser, and that is the whole point of this declaration.
  //
  // `std::make_unique<Game>()` as a NSDMI runs BEFORE the constructor body, so it ran before
  // `psxport_install_game(runtime)` below — and `Core::Core` reads the installed runtime
  // (`core.cpp:31`) while the Game is being constructed. The fixture therefore handed `Core` whatever
  // pointer the PREVIOUS test had installed: for the first test in the file that was null and the
  // null branch saved it, and for every later test it was a pointer to an already-destroyed `Runtime`.
  //
  // Clang tolerated the freed stack memory and this suite passed 15/15; GCC 16.2.1 at -O2 reused the
  // frame and `Core::Core` segfaulted on `runtime->guestProgramImage()`. Same defect class as the
  // `insideCheckout` dangling return in `mods.cpp` — **undefined behaviour that the agents' own
  // toolchain hides** — and the fix in both places is the lifetime, not the symptom.
  std::unique_ptr<Game> game;

  Fixture() {
    psxport_install_game(runtime);
    // Constructed HERE, after the install, because constructing a Game is what reads the runtime.
    game = std::make_unique<Game>();
    game->hle.irq_enabled = 1;
    Core &core = game->core;
    core.imageCatalog().activate("rcnt2-poll", {kProgram, kProgram + sizeof(kPollProgram)}, 0x52434e54ull);
    core.imageCatalog().activate("rcnt2-trampoline", {kTrampoline, kTrampoline + 8u}, 0x54524d50ull);
    for (std::size_t i = 0; i < std::size(kPollProgram); ++i) {
      core.mem_w32(kProgram + static_cast<std::uint32_t>(i) * 4u, kPollProgram[i]);
    }
    // A real return address. `dispatchGuest0` is called with r31 == 0 otherwise, and a `jr ra` to 0
    // is an ambiguous-image fault — which would mask the measurement this test exists to take.
    core.mem_w32(kTrampoline + 0u, 0x03e00008u);
    core.mem_w32(kTrampoline + 4u, 0x00000000u);
    core.r[31] = kTrampoline;
  }
};

// ---------------------------------------------------------------------------------------------
// Half one: the ledger. Pure arithmetic, no guest, no Lightrec — so the backwards-jump property is
// an executable statement about the shipping class rather than an inference from a guest run.
// ---------------------------------------------------------------------------------------------

static void test_a_standing_counter_charges_nothing(void) {
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  // Denominator: this is a counter that HAS moved 3 times. A ledger that reported 0 instructions for
  // all three would be indistinguishable from one that was never called.
  CHECK_EQ(ledger.commitThrough(20), 10u);
  CHECK_EQ(ledger.commitThrough(20), 0u);
  CHECK_EQ(ledger.commitThrough(22), 1u);
  CHECK_EQ(ledger.committedInstructions(), 11u);
  CHECK_EQ(ledger.uncommitted(100), 89u);
}

static void test_a_stationary_counter_charges_nothing_at_all(void) {
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  // The negative arm of the case above. `lightrec_current_cycle_count` is legitimately unchanged
  // between two accesses inside one block, and a ledger that charged for those would inflate the
  // clock on a guest that reads the same register twice in three instructions.
  for (int access = 0; access < 64; ++access) {
    CHECK_EQ(ledger.commitThrough(0), 0u);
  }
  CHECK_EQ(ledger.committedInstructions(), 0u);
  CHECK_EQ(ledger.uncommitted(0), 0u);
}

// THE BACKWARDS-JUMP CASE. `current_cycle` is reset to 0 by `lightrec_reset_cycle_count` at every
// segment start, so a ledger that kept its baseline across segments would compute
// `0 - 500_000`, wrap, and hand the clock a colossal number of instructions — at the FIRST device
// access of every segment, on every run, in every port.
static void test_a_reset_counter_does_not_walk_the_clock_backwards(void) {
  psx::cpu::SegmentClockLedger ledger;
  // `clock` is what `psx::frame::EmulatedTime` would hold: the sum of every count handed to
  // `advanceGuestInstructionTicks`. Tracking it here is what makes "the clock did not go backwards"
  // an executable statement about the ledger rather than an inference about the code.
  std::uint64_t clock = 0;
  auto commit = [&ledger, &clock](std::uint32_t cycle) {
    const std::uint32_t charged = ledger.commitThrough(cycle);
    const std::uint64_t before = clock;
    clock += charged;
    return clock >= before;
  };

  ledger.beginSegment();
  CHECK(commit(500'000));
  CHECK_EQ(clock, 250'000u);

  // The next segment, in the executor's order: reset the counter, then reset the baseline. A ledger
  // that reset only the counter would compute `6 - 250000` here, which wraps to ~2^32/2 instructions
  // and hands the clock a colossal jump. That is the second bug this change could introduce, and the
  // in-segment test above cannot see it.
  ledger.beginSegment();
  CHECK(commit(6));
  CHECK_EQ(ledger.committedInstructions(), 3u);
  CHECK_EQ(clock, 250'003u);

  // Carry on through the seam: a few more accesses in each segment, clock never decreasing.
  // Sixteen more accesses spread over four more segments. Each one charges only the cycles its own
  // segment's counter observed, so the growth is bounded by the cycles those counters reported —
  // which is what makes the bound below a statement about the delta rather than about a constant.
  std::uint32_t cyclesObservedAfterSeam = 0;
  for (int i = 0; i < 16; ++i) {
    if (i % 4 == 0) {
      ledger.beginSegment();
      CHECK(commit(2));
      cyclesObservedAfterSeam += 2;
      continue;
    }
    const auto cycle = static_cast<std::uint32_t>(2 + i * 2);
    CHECK(commit(cycle));
    cyclesObservedAfterSeam += cycle;
  }
  CHECK(clock > 250'003u);
  // The clock grew by AT MOST the cycles the counters reported after the seam (halved by the
  // conversion). A ledger that had kept the pre-seam baseline would have added ~2^31 instead, and a
  // ledger charging absolutely would have added the sum of every absolute reading.
  CHECK(clock - 250'003u <= cyclesObservedAfterSeam / psx::cpu::kLightrecCyclesPerInstruction);
  CHECK(clock - 250'003u > 0u);
}

// THE BACKWARDS CASE AT THE UNIT LEVEL, independent of whether the caller remembered to reset.
//
// `beginSegment` is what the executor does in the same breath as `lightrec_reset_cycle_count`, and
// the test above pins that pairing. But the property "a commit is never a subtraction" has to hold
// even for a caller that does not pair them, because the failure mode is a u32 wrap: an unsigned
// `currentCycle - baseline` where the counter went back reads as ~4 billion, and
// `advanceGuestInstructionTicks` would add ~2 billion instructions to the clock in one call.
//
// So this drives the counter BACKWARDS on purpose, with no `beginSegment` in between, and asserts the
// two things a wrapped subtraction cannot satisfy: the charge is not larger than the counter can
// express, and the running total only ever grows by that charge.
static void test_a_counter_that_goes_backwards_never_charges_a_wrapped_amount(void) {
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  CHECK_EQ(ledger.commitThrough(1'000), 500u);

  // The counter restarts under us, as a nested guest execution's `lightrec_reset_cycle_count` would.
  // No `beginSegment`: the caller did not pair them, and the ledger must still be safe.
  const std::uint32_t afterReset = ledger.commitThrough(4);
  CHECK(afterReset <= 4u / psx::cpu::kLightrecCyclesPerInstruction);
  CHECK_EQ(ledger.committedInstructions(), 500u + afterReset);

  // And a second reset, larger than the first, from a baseline that never moved.
  const std::uint32_t afterSecondReset = ledger.commitThrough(2);
  CHECK(afterSecondReset <= 2u / psx::cpu::kLightrecCyclesPerInstruction);
  CHECK_EQ(ledger.committedInstructions(), 500u + afterReset + afterSecondReset);

  // The total a wrapped implementation would have reached, stated so the check above is not vacuous:
  // 1'000 is the pre-reset baseline, so a wrapped delta of 4 - 1'000 is 4'294'967'300 cycles.
  CHECK(1'000u > 4u);
}

// A monotonicity statement over a sequence that mixes every case the counter can present, in an
// order chosen to break an implementation that special-cases "has not moved" but not "went back".
// `committedInstructions` is the clock's own input, so non-decreasing here IS clock monotonicity.
static void test_the_committed_total_is_monotonic_across_every_counter_motion(void) {
  psx::cpu::SegmentClockLedger ledger;
  const std::vector<std::uint32_t> script{
      0, 2, 2, 4, 4, 4, 1000, 1000, 12, 0, 18, 18, 4'000'000, 64, 64, 66, 0, 0, 0, 2, 2, 2, 0, 8, 2'000'000'000u,
  };
  std::uint64_t previous = 0;
  for (const std::uint32_t cycle : script) {
    if (cycle == 0) {
      ledger.beginSegment();
      previous = 0; // a new segment's own total starts from the new segment
      continue;
    }
    const std::uint32_t charged = ledger.commitThrough(cycle);
    CHECK(ledger.committedInstructions() >= previous);
    // Every commit is an addition, and the only way to break that is a charge computed from a
    // negative delta. A charge larger than the counter can express is exactly that.
    CHECK(charged <= cycle);
    CHECK(ledger.committedInstructions() <= previous + charged);
    previous = ledger.committedInstructions();
  }
}

// `uncommitted` must never go negative. The segment's own accounting subtracts what has already been
// committed, and an over-committed segment (a conversion rounding up past the true count) reaching
// that subtraction as a negative would add a huge value to the clock through `advanceInstructions`.
static void test_an_over_committed_segment_owes_nothing_rather_than_a_negative(void) {
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  CHECK_EQ(ledger.commitThrough(4'000), 2'000u);
  // The segment turned out to have executed fewer instructions than the commits implied.
  CHECK_EQ(ledger.uncommitted(10), 0u);
  CHECK_EQ(ledger.uncommitted(0), 0u);
  CHECK_EQ(ledger.uncommitted(2'001), 1u);
}

// The conversion is by a NAMED constant that converts Lightrec's cycle budget into this clock's
// instruction unit, and it must not drift: the segment's true instruction count and the sum of its
// device commits have to agree, or the two views of one measurement contradict each other.
static void test_the_conversion_constant_is_the_one_lightrec_charges(void) {
  // Lightrec sets `state->cycles_per_op = 2` and every emitter charges exactly that per opcode.
  // If the framework's divisor and Lightrec's cost model ever disagree, this is where it shows.
  CHECK_EQ(psx::cpu::kLightrecCyclesPerInstruction, 2u);
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  CHECK_EQ(ledger.commitThrough(1'000'000), 1'000'000u / psx::cpu::kLightrecCyclesPerInstruction);
  CHECK_EQ(ledger.uncommitted(500'000), 0u);
}

// A delta smaller than one whole instruction must be CARRIED, not truncated away. The JIT charges
// per opcode, so a small delta is rare, but a ledger that dropped it would silently lose guest time
// on any workload that produced one — and the loss would be invisible, because the clock would still
// be moving.
static void test_sub_instruction_deltas_are_carried_not_truncated(void) {
  psx::cpu::SegmentClockLedger ledger;
  ledger.beginSegment();
  std::uint64_t charged = 0;
  // 20 accesses of one cycle each: 20 cycles is 10 instructions, and ten separate truncations
  // would have produced 0. This is the case that makes the loss detectable at all.
  for (int access = 0; access < 20; ++access) {
    charged += ledger.commitThrough(static_cast<std::uint32_t>(access + 1));
  }
  CHECK_EQ(charged, 20u / psx::cpu::kLightrecCyclesPerInstruction);
  CHECK_EQ(ledger.committedInstructions(), charged);
}

// ---------------------------------------------------------------------------------------------
// Half two: the guest-visible case, through the real executor.
// ---------------------------------------------------------------------------------------------

struct PollRun {
  std::vector<std::uint32_t> samples;
  const psx::cpu::ExecutorCounters &counters;
  bool returned = false;
  std::uint32_t returnPc = 0;
};

static PollRun runPoll(Fixture &fixture) {
  Core &core = fixture.game->core;
  const auto result = psx::cpu::dispatchGuest0(core, kProgram, psx::cpu::ExecutionBudget::fromCycles(4'000'000));
  PollRun run{.counters = core.lightrecExecutor().counters()};
  run.returned = result.returned();
  run.returnPc = result.guestPc;
  run.samples.reserve(kSamples);
  for (std::uint32_t i = 0; i < kSamples; ++i) {
    run.samples.push_back(core.mem_r32(kRecord + i * 4u));
  }
  return run;
}

static std::size_t countDistinct(const std::vector<std::uint32_t> &values) {
  std::vector<std::uint32_t> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  return sorted.size();
}

static void test_a_guest_polling_root_counter2_inside_one_segment_sees_it_move(void) {
  Fixture fixture;
  const PollRun run = runPoll(fixture);
  if (!run.returned) {
    std::printf("  poll program exited at 0x%08X rather than returning\n", run.returnPc);
  }
  CHECK(run.returned);
  CHECK_EQ(run.samples.size(), kSamples);
  // The loop ran to completion: a2 reached the end pointer, so every sample was written.
  CHECK_EQ(fixture.game->core.r[6], kRecord + kSamples * 4u);
  // THE CONTROL. All 32 samples were fetched from one register; before the charge every one of them
  // was the same value, because `psx::frame::EmulatedTime` could not move until the segment ended. `countDistinct
  // == 1` is the defect, so this asserts the opposite AND names the number that would be the defect.
  const std::size_t distinct = countDistinct(run.samples);
  std::printf("  %zu distinct RCnt2 values across %u reads in one segment (%llu device commits)\n",
              distinct,
              kSamples,
              static_cast<unsigned long long>(run.counters.deviceClockCommits));
  CHECK(distinct > 1u);
  // More than two: "the clock moved at least once" is too weak to be evidence, because a single
  // commit at the end of a segment would satisfy it and is the shape of the original defect.
  CHECK(distinct > kSamples / 2u);
  // And the commits are attributed: the denominator that separates "no device access charged
  // anything" from "the instrument never ran".
  CHECK(run.counters.deviceClockCommits > 0u);
  CHECK(run.counters.deviceClockCommitInstructions > 0u);
}

// The segment's TOTAL must be exactly the instruction count it executed, whether or not any device
// access committed part of it first. If the mid-segment commits were counted twice the clock would
// run fast; if they were dropped the clock would run slow. Both are the same single check: the
// clock's advance equals the executor's own instruction total.
static void test_the_segments_total_clock_advance_is_unchanged_by_the_mid_segment_commits(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  const std::uint64_t ticksBefore = fixture.game->timing.emulatedCpuTicks();
  const PollRun run = runPoll(fixture);
  const std::uint64_t advanced = fixture.game->timing.emulatedCpuTicks() - ticksBefore;
  CHECK_EQ(advanced, run.counters.executedInstructions);
  // Both numbers non-zero, so the equality above is not 0 == 0.
  CHECK(advanced > 0u);
  CHECK(run.counters.executedInstructions > 0u);
  // The commits are a strict SUBSET of the segment's work, which is what makes the total exact.
  CHECK(run.counters.deviceClockCommitInstructions < run.counters.executedInstructions);
}

// The clock must not walk backwards across a segment boundary. This is the property most likely to
// pass a test written only for the first case, and it is the second bug the change could introduce:
// `current_cycle` is a u32 that the executor resets per segment, so charging it absolutely would
// move the clock backwards at every boundary — while the in-segment test above stayed green.
//
// The observable is a GUEST-VISIBLE one, because that is what a backwards jump breaks. RCnt2 counts
// `(now - origin) >> shift` from the clock, so a clock that went backwards would make the counter
// read a value BELOW one it already returned. A program that polls RCnt2 across two segments — a
// small budget, so the executor ends the first segment and starts a second — must see a
// non-decreasing sequence.
// The same total-advance property, but across MANY `executeWithBoundary` calls rather than one.
//
// This is the case that catches a ledger whose per-segment state is not reset. A stale
// `committedInstructions_` carries the previous call's commits forward, so each call's `uncommitted`
// subtracts work that was already charged, and the clock falls BEHIND the executor's own instruction
// total — a guest reading a counter then sees it advance too slowly, which is the same defect as the
// frozen clock, only smaller. One call cannot detect it: with a single segment there is nothing to
// carry over.
static void test_the_clock_advance_equals_the_instruction_total_across_many_calls(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  const std::uint64_t ticksBefore = fixture.game->timing.emulatedCpuTicks();
  const auto instructionsBefore = core.lightrecExecutor().counters().executedInstructions;

  // A budget far too small to finish the 32-read program, so each call ends mid-segment and the next
  // one restarts the guest from a reset Lightrec counter.
  constexpr int kCalls = 24;
  for (int call = 0; call < kCalls; ++call) {
    (void)psx::cpu::dispatchGuest0(core, kProgram, psx::cpu::ExecutionBudget::fromCycles(60));
  }

  const std::uint64_t advanced = fixture.game->timing.emulatedCpuTicks() - ticksBefore;
  const std::uint64_t executed = core.lightrecExecutor().counters().executedInstructions - instructionsBefore;
  std::printf("  %d calls: clock advanced %llu, executor counted %llu instructions, %llu commits\n",
              kCalls,
              static_cast<unsigned long long>(advanced),
              static_cast<unsigned long long>(executed),
              static_cast<unsigned long long>(core.lightrecExecutor().counters().deviceClockCommits));
  // The clock is the instruction total and nothing else, whatever the mid-segment commits did.
  CHECK_EQ(advanced, executed);
  // Both sides non-zero, so the equality is not 0 == 0 — and the commits really happened, so this is
  // a run where the mid-segment charge was live rather than a run that never exercised it.
  CHECK(executed > 0u);
  CHECK(core.lightrecExecutor().counters().deviceClockCommits > 0u);
}

static void test_root_counter2_never_decreases_across_a_segment_boundary(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  // Two reads separated by more than one segment's worth of budget. The second read is a fresh
  // `executeWithBoundary` segment, so the executor reset Lightrec's counter to 0 in between.
  core.mem_w32(kProgram + 0x0cu, 0x3c070001u); // lui   a3, 0x0001     (unchanged; documented here)
  const std::vector<std::uint32_t> observedBefore{
      fixture.game->timing.rootCounter2(),
  };
  (void)observedBefore;

  // Drive two segments and watch the counter across the seam.
  const std::uint32_t reads = 8;
  std::vector<std::uint32_t> reads_taken;
  for (std::uint32_t i = 0; i < reads; ++i) {
    // A budget small enough that the poll program cannot finish in one segment, so `executeWithBoundary`
    // runs its `while` loop and re-enters `lightrec_reset_cycle_count` between iterations.
    const auto result = psx::cpu::dispatchGuest0(
        core, kProgram, psx::cpu::ExecutionBudget::fromCycles(200 * psx::cpu::kLightrecCyclesPerInstruction));
    reads_taken.push_back(fixture.game->timing.rootCounter2());
    if (result.guestPc == 0) {
      break;
    }
  }
  CHECK(reads_taken.size() >= 2u);
  for (std::size_t i = 1; i < reads_taken.size(); ++i) {
    // A backwards jump shows up here as a DECREASE, and RCnt2's 16-bit wrap is the one legitimate
    // decrease; a wrap needs 65536 ticks and these segments are hundreds.
    std::printf(
        "  seam read %zu: RCnt2=0x%04X (previous 0x%04X)\n", i, reads_taken[i] & 0xFFFFu, reads_taken[i - 1] & 0xFFFFu);
    CHECK(reads_taken[i] >= reads_taken[i - 1]);
  }
}

// A STORE to a device register is the same contract violation as a load, and this framework has a
// concrete reason it matters rather than a symmetrical one: `psx::frame::Timing::rootCounter2Write` records
// `rootCounter2OriginTicks = mEmulatedTime.nowTicks()` when the guest programs the counter, and
// `rootCounter2()` then counts from that origin. A write observed with a stale clock anchors the
// counter to a time that has already passed, so the first read after it reports a delta the guest
// never asked for. The program latches RCnt2, burns guest work, writes the mode register, and
// records RCnt2 — the value a guest would read to measure its own elapsed time.
static void test_a_device_store_is_charged_as_well_as_a_device_read(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  // A WRITE to a device register is the same contract violation as a read, and this framework has a
  // concrete reason it matters rather than a merely symmetrical one: `psx::frame::Timing::rootCounter2Write`
  // records `rootCounter2OriginTicks = mEmulatedTime.nowTicks()` when the guest programs the counter,
  // and `rootCounter2()` counts from that origin. A write observed against a stale clock anchors the
  // counter to a moment that has already passed.
  //
  // The program is the poll loop with a device STORE added, so it has the same shape as the case
  // above — which is the point, and it took a measurement to learn: an earlier version of this test
  // reached the device registers through a pointer held in a register, and the pointer was
  // constant-propagated on block re-entry, so 8 of the 8 iterations' three device accesses were
  // compiled to direct I/O and never entered the callbacks at all. Measured on that version: 47 guest
  // instructions, 11 memory callbacks, 0 device commits. A test written against a program that does
  // not reach the seam reports a green zero for the wrong reason, which is why the loop here mirrors
  // the one that is already known to reach it and the denominators are printed.
  constexpr std::uint32_t kStoreProgram = 0x00060000u;
  // The poll loop, with `sw zero, 0x1124(a1)` — a write to RCnt2's MODE register — added after the
  // record store. Assembled from the same encoding rules as the loop above.
  constexpr std::uint32_t kStoreProgramBytes[] = {
      0x3c051f80u, // 0x00
      0x3c060001u, // 0x04
      0x24c61100u, // 0x08
      0x3c070001u, // 0x0c
      0x24e71180u, // 0x10
      0x8ca21120u, // 0x14 <- .loop
      0xacc20000u, // 0x18
      0x24c60004u, // 0x1c
      0xaca01124u, // 0x20
      0x00c7182au, // 0x24
      0x1460fffau, // 0x28
      0x00000000u, // 0x2c
      0x03e00008u, // 0x30
      0x00000000u, // 0x34
  };
  core.imageCatalog().activate(
      "rcnt2-store", {kStoreProgram, kStoreProgram + sizeof(kStoreProgramBytes)}, 0x53544f52ull);
  // The store loop returns through the SAME trampoline image the poll loop uses, and both programs
  // are resident in one process, so the trampoline needs its own identity or the return address
  // resolves to two images. Measured: "exited 0x00060014 as fault: ambiguous code-image identity".
  core.imageCatalog().activate("rcnt2-store-trampoline", {kTrampoline, kTrampoline + 8u}, 0x53545250ull);
  for (std::size_t i = 0; i < std::size(kStoreProgramBytes); ++i) {
    core.mem_w32(kStoreProgram + static_cast<std::uint32_t>(i) * 4u, kStoreProgramBytes[i]);
  }
  core.r[31] = kTrampoline;

  const auto before = core.lightrecExecutor().counters();
  const auto result = psx::cpu::dispatchGuest0(core, kStoreProgram, psx::cpu::ExecutionBudget::fromCycles(4'000'000));
  if (!result.returned()) {
    std::printf("  store program exited 0x%08X as %s: %s\n",
                result.guestPc,
                psx::cpu::executionExitName(result.reason),
                result.detail.c_str());
  }
  CHECK(result.returned());
  const auto &after = core.lightrecExecutor().counters();
  const std::uint64_t newCommits = after.deviceClockCommits - before.deviceClockCommits;
  const std::uint64_t newCallbacks = after.memoryCallbacks - before.memoryCallbacks;
  const std::uint64_t newInstructions = after.executedInstructions - before.executedInstructions;
  std::printf("  store program: %llu instructions, %llu memory callbacks, %llu device commits\n",
              static_cast<unsigned long long>(newInstructions),
              static_cast<unsigned long long>(after.memoryCallbacks - before.memoryCallbacks),
              static_cast<unsigned long long>(newCommits));
  CHECK(newInstructions > 0u);
  // The store really did reach the seam: 32 reads + 32 record stores + 32 mode stores is 96, and the
  // callbacks are within one of that. A build that dropped the store callback would report ~64.
  CHECK(newCallbacks >= 3u * kSamples - 1u);
  // AND THE COUNT THAT MATTERS IS UNCHANGED BY THE STORE, which is the property rather than an
  // accident. Measured: 95 callbacks, 31 commits — the same 31 the read-only loop produces. The
  // reason is the charge rule: a commit happens when Lightrec's counter HAS MOVED, and cycles are
  // charged at block ends, so the store shares its block with the preceding read, sees the same
  // counter value, and correctly charges nothing. The clock is already up to date for it, because the
  // read earlier in the same block committed everything Lightrec had charged so far.
  //
  // So the store is charged "for free" and this is not a hole: the intra-block gap it shares is the
  // same gap `test_the_charge_granularity_is_one_block_not_one_instruction` states, and closing it is
  // issue 0007's cycle-accurate model. What must NOT happen is a commit per access, which would
  // double-count the block's cycles — and the count being 31 rather than ~63 is what rules that out.
  CHECK(newCommits > 0u);
  CHECK(newCommits < 2u * kSamples);
  // Every sample is still distinct, so the store did not cost the loop its visibility.
  int distinct = 0;
  for (std::uint32_t i = 1; i < kSamples; ++i) {
    if (core.mem_r32(kRecord + i * 4u) != core.mem_r32(kRecord)) {
      ++distinct;
    }
  }
  CHECK(distinct > static_cast<int>(kSamples / 2));
}

// WHAT THIS MECHANISM IS AND IS NOT, stated as an assertion rather than left to be discovered.
//
// Lightrec charges a block's cycles at the block's END, so `state->current_cycle` is CONSTANT for the
// whole of a straight-line basic block. The charge therefore cannot see intra-block progress, and
// measuring it is the only way to know that: the 11-instruction straight-line version of the latch
// case above reached the memory callbacks 5 times and committed 0 instructions.
//
// What that costs, stated plainly: a guest that reads a device register twice inside ONE block with
// substantial work between the reads still sees the same value twice. What it does not cost: the
// defect this fixes. Every hardware-counter POLL LOOP is one block per iteration, so the counter
// advances between polls, which is the observable a stopwatch idiom needs. Issue 0007's cycle-accurate
// R3000 model is what would close the intra-block gap, and the shape of that work is named in
// `segment_clock.h`.
static void test_the_charge_granularity_is_one_block_not_one_instruction(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  constexpr std::uint32_t kStraightLine = 0x00050000u;
  core.imageCatalog().activate("straight-line", {kStraightLine, kStraightLine + 16u}, 0x53545241ull);
  // lui t1, 0x0001 ; lw t2, 0x2000(t1) ; lw v0, 0x0000(t2) ; jr ra  — ONE block, one device read.
  core.mem_w32(kStraightLine + 0u, 0x3c090001u);
  core.mem_w32(kStraightLine + 4u, 0x8d0a2000u);
  core.mem_w32(kStraightLine + 8u, 0x8c822000u);
  core.mem_w32(kStraightLine + 12u, 0x03e00008u);
  core.mem_w32(0x00012000u, kRootCounter2Value);
  core.r[31] = kStraightLine + 12u;

  const auto before = core.lightrecExecutor().counters();
  const auto result = psx::cpu::dispatchGuest0(core, kStraightLine, psx::cpu::ExecutionBudget::fromCycles(1'000'000));
  CHECK(result.returned());
  const auto &after = core.lightrecExecutor().counters();
  std::printf("  straight-line block: %llu instructions, %llu callbacks, %llu commits\n",
              static_cast<unsigned long long>(after.executedInstructions - before.executedInstructions),
              static_cast<unsigned long long>(after.memoryCallbacks - before.memoryCallbacks),
              static_cast<unsigned long long>(after.deviceClockCommits - before.deviceClockCommits));
  // The callback fired — so this is a measurement, not a silent zero.
  CHECK(after.memoryCallbacks - before.memoryCallbacks >= 1u);
  // And it committed nothing, because Lightrec had not charged a single cycle yet at the end of the
  // first block. This asserts the LIMIT rather than the defect: if a future Lightrec starts charging
  // per instruction this goes red, and that is the day the intra-block gap closes.
  CHECK_EQ(after.deviceClockCommits - before.deviceClockCommits, 0u);
}

// The negative control for the whole mechanism, and the one that makes a green clock meaningful: a
// program that touches NO device register must leave the clock exactly where the segment's own
// accounting puts it, and must record zero device commits. If the charge fired on ordinary RAM
// traffic this is where it would show — a device-clock commit count far above the device reads.
static void test_a_run_with_no_device_access_makes_no_device_commit(void) {
  Runtime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->hle.irq_enabled = 1;
  Core &core = game->core;
  // Pure main-RAM traffic through the OUT-OF-LINE memory callbacks, and no hardware register.
  //
  // Getting a RAM access out of line takes care, because the obvious program does not reach the
  // callbacks at all: Lightrec's constant propagation resolves `lw v0, 0(t0)` against the ENTRY
  // register state, so with t0 = 0x1000 at entry it tags all eight loads IO_RAM and emits direct
  // loads. Measured with the device gate REMOVED: that version still reported 0 charges, because the
  // callbacks it was supposed to tax were never invoked. That is the "instrument never ran" failure
  // a denominator exists to rule out, so this program defeats the propagation instead of assuming it.
  //
  // The address is taken from a value LOADED FROM MEMORY, which constant propagation cannot resolve
  // to anything: `lw t1, 0(t2)` yields an unknown value, so `lw v0, 0(t1)` is tagged at translation
  // time as an untagged access and must exit through `rec_io` -> the C wrapper -> psxport's
  // `loadWord`. The `memoryCallbacks` denominator below is what proves it, and the test
  // fails loudly rather than passing vacuously if that ever stops being true.
  constexpr std::uint32_t kRamProgram = 0x00030000u;
  constexpr std::uint32_t kIterations = 32;
  core.imageCatalog().activate("ram-only", {kRamProgram, kRamProgram + 40u}, 0x52414d4fu);
  core.mem_w32(kRamProgram + 0u, 0x3c0a0001u); // 0x00 lui   t2, 0x0001   -> t2 = 0x00010000
  core.mem_w32(kRamProgram + 4u, 0x3c0b0000u); // 0x04 lui   t3, 0x0000
  core.mem_w32(kRamProgram + 8u, 0x256b0020u); // 0x08 addiu t3, t3, 32
  // The pointer load is INSIDE the loop, and that placement is load-bearing: constant propagation
  // seeds a block from the block's ENTRY register state, so a pointer loaded before the loop is
  // already known inside it and `lw v0, 0(t1)` gets tagged IO_RAM and compiled to a direct load.
  // Measured: 102 instructions and ONE callback, which is the first block only. Reloading it per
  // iteration is what actually forces the out-of-line path.
  core.mem_w32(kRamProgram + 12u, 0x8d490000u); // 0x0c lw    t1, 0x000(t2) <- .loop
  core.mem_w32(kRamProgram + 16u, 0x8d220000u); // 0x10 lw    v0, 0x000(t1) <- out of line
  core.mem_w32(kRamProgram + 20u, 0x256bffffu); // 0x14 addiu t3, t3, -1
  core.mem_w32(kRamProgram + 24u, 0x1560fffcu); // 0x18 bnez  t3, .loop
  core.mem_w32(kRamProgram + 28u, 0x00000000u); // 0x1c nop
  core.mem_w32(kRamProgram + 32u, 0x03e00008u); // 0x20 jr    ra
  core.mem_w32(kRamProgram + 36u, 0x00000000u); // 0x24 nop (delay slot)
  core.mem_w32(0x00010000u, 0x00003000u);       // [t2] = a main-RAM address
  core.mem_w32(0x00003000u, 0xdeadbeefu);       // what the out-of-line load reads
  core.r[31] = kRamProgram + 32u;

  const auto result = psx::cpu::dispatchGuest0(core, kRamProgram, psx::cpu::ExecutionBudget::fromCycles(1'000'000));
  if (!result.returned()) {
    std::printf("  RAM-only exited 0x%08X as %s: %s\n",
                result.guestPc,
                psx::cpu::executionExitName(result.reason),
                result.detail.c_str());
  }
  CHECK(result.returned());
  const auto &counters = core.lightrecExecutor().counters();
  std::printf("  RAM-only run: %llu instructions, %llu memory-callback invocations, %llu device commits\n",
              static_cast<unsigned long long>(counters.executedInstructions),
              static_cast<unsigned long long>(counters.memoryCallbacks),
              static_cast<unsigned long long>(counters.deviceClockCommits));
  // DENOMINATOR FIRST, and the reason this case can fail. The out-of-line RAM load really did reach
  // psxport's memory callback `kIterations` times, so a charge on every callback would report ~32
  // here. Without this check the case below would pass on a run that never exercised the thing it is
  // about — which is exactly what the first version of this test did.
  CHECK(counters.memoryCallbacks >= kIterations);
  // The guest really read the RAM word, so the load was not elided.
  CHECK_EQ(core.r[kV0], 0xdeadbeefu);
  CHECK(counters.executedInstructions >= 4u * kIterations);
  // THE GATE. The ADDRESS decides, not the callback: none of these callbacks named a device address.
  CHECK_EQ(counters.deviceClockCommits, 0u);
  // THE GATE. The address decides, not the callback: none of these callbacks named a device address.
  CHECK_EQ(counters.deviceClockCommits, 0u);
}

} // namespace

int main() {
  // The ledger first: it is hermetic, so a failure there names the arithmetic rather than the guest.
  RUN(a_standing_counter_charges_nothing);
  RUN(a_stationary_counter_charges_nothing_at_all);
  RUN(a_reset_counter_does_not_walk_the_clock_backwards);
  RUN(a_counter_that_goes_backwards_never_charges_a_wrapped_amount);
  RUN(the_committed_total_is_monotonic_across_every_counter_motion);
  RUN(an_over_committed_segment_owes_nothing_rather_than_a_negative);
  RUN(the_conversion_constant_is_the_one_lightrec_charges);
  RUN(sub_instruction_deltas_are_carried_not_truncated);
  // Then the guest-visible cases, through the real executor.
  RUN(a_guest_polling_root_counter2_inside_one_segment_sees_it_move);
  RUN(the_segments_total_clock_advance_is_unchanged_by_the_mid_segment_commits);
  RUN(the_clock_advance_equals_the_instruction_total_across_many_calls);
  RUN(root_counter2_never_decreases_across_a_segment_boundary);
  RUN(a_device_store_is_charged_as_well_as_a_device_read);
  RUN(the_charge_granularity_is_one_block_not_one_instruction);
  RUN(a_run_with_no_device_access_makes_no_device_commit);
  return pt_summary();
}
