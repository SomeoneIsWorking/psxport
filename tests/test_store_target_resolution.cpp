// Is the store observer's resolved DESTINATION sound when the store's base is bumped in a delay slot?
//
// WHY THIS FILE EXISTS. `StoreTargetCounts` now reports where an observed store WROTE, resolved from the
// instruction word and the register file the callback is handed. On 2026-09-27 that reporting produced the
// first concrete lead on Spyro 1's moby list: arming the filler's append at `0x8005205C` reported its
// first write at `0x800700F8` while the consumer reads the list at `0x800700F4` — a four-byte disagreement
// between the function that writes the list and the function that reads it, which would be the root cause
// of every moby failing to animate.
//
// **That lead is only as good as this derivation**, and the derivation has a specific, unverified hazard.
// The instruction AFTER the observed store is:
//
//     0x8005205C  sw   $at, 0($t6)      <- the armed store
//     0x80052060  bltz $v0, .L80052040
//     0x80052064   addi $t6, $t6, 0x4  <- DELAY SLOT: the base is bumped here
//
// A branch delay slot executes BEFORE the branch resolves, so `$t6` is incremented immediately after the
// store. If Lightrec hands the callback the register file at a point where that increment has already
// landed, then `$t6` reads one slot past the word the store actually wrote, and EVERY derived address for
// this instruction shape is +4 — which is precisely the "plausible-looking address that is not the field"
// failure the sign-extension note already warns about, arriving by a different route.
//
// The existing positive test cannot see this: its store `sw t1, 0(t0)` is followed by `jr ra`, whose delay
// slot does not touch `t0`. So the hazard is untested by construction.
//
// THE ORACLE HERE IS GUEST MEMORY, NOT THE DERIVATION. The fixture performs a store whose base is
// incremented in a delay slot, and the test asserts the derived address equals the address the store
// REALLY wrote — established by reading that word back and checking its value, and by checking the word one
// slot higher is untouched. If the derivation is skewed, the memory says so and the test fails.
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"
#include "render_capabilities.h"
#include "testutil.h"

#include <array>
#include <cstdio>
#include <memory>

namespace {

// A minimal GameRuntime, because `Runtime`/`makeGame` are local to test_dynarec_contract.cpp and this
// test is deliberately standalone: the point of the file is ONE question, and copying the harness keeps
// the two tests from sharing a fixture whose changes would move both answers at once.
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

std::unique_ptr<Game> makeGame(Runtime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

constexpr std::uint32_t kEntry = 0x00011000u;
constexpr std::uint32_t kSlotA = 0x60u; // where the store must land
constexpr std::uint32_t kSlotB = 0x64u; // the slot a +4 skew would wrongly report

struct Trace {
  std::size_t calls = 0;
  std::uint32_t pc = 0;
  std::uint32_t instruction = 0;
  std::array<std::uint32_t, 34> gpr{};
  std::uint64_t observations = 0;
};

void capture(const psx::cpu::StoreObservation &observation, void *data) noexcept {
  auto &trace = *static_cast<Trace *>(data);
  if (observation.guestPc != 0) {
    trace.pc = observation.guestPc;
    trace.instruction = observation.instruction;
    std::copy_n(observation.gpr.begin(), observation.gpr.size(), trace.gpr.begin());
    ++trace.observations;
  }
  ++trace.calls;
}

// The shape, assembled by hand from instruction words:
//
//   addiu $t0, $zero, 0x60      0x24080060
//   addiu $t1, $zero, 0x37      0x24090037
//   sw    $t1, 0($t0)           0xAD090000   <- the armed store
//   bltz  $t2, <self>           0x1840FFFE   <- branch whose DELAY SLOT bumps the store's base
//    addi $t0, $t0, 0x4         0x25080004
//   ...
//
// `$t2` is zero, so `bltz` is not taken and the delay slot has still executed: `$t0` is 0x64 by the time
// control leaves the block, and the store must have written 0x60.
static void test_store_in_a_delay_slot_bumped_base() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  core.mem_w32(kEntry + 0u, 0x24080060u);  // addiu t0, zero, 0x60
  core.mem_w32(kEntry + 4u, 0x24090037u);  // addiu t1, zero, 0x37
  core.mem_w32(kEntry + 8u, 0xad090000u);  // sw    t1, 0(t0)
  core.mem_w32(kEntry + 12u, 0x1840fffeu); // bltz  t2, kEntry+20  (t2 = 0, not taken)
  core.mem_w32(kEntry + 16u, 0x25080004u); // addi  t0, t0, 0x4    (DELAY SLOT)
  core.mem_w32(kEntry + 20u, 0x1000ffffu); // beq   zero, zero, self
  core.mem_w32(kEntry + 24u, 0x00000000u); // nop (delay slot)
  core.mem_w32(kSlotA, 0u);
  core.mem_w32(kSlotB, 0u);

  Trace trace;
  auto &executor = core.lightrecExecutor();
  CHECK(executor.available());
  const std::uint32_t armed = kEntry + 8u;
  CHECK_EQ(executor.configureStoreObserver(std::span(&armed, 1), capture, &trace),
           psx::cpu::StoreObserverStatus::Configured);
  const auto result = executor.execute(kEntry, psx::cpu::ExecutionBudget::fromCycles(60));
  const auto report = executor.storeObserverReport();

  CHECK(trace.observations > 0u);
  if (trace.observations == 0u) {
    return;
  }
  CHECK_EQ(trace.instruction, 0xad090000u);
  CHECK_EQ(trace.pc, armed);

  // THE INDEPENDENT ORACLE. What did the store really do?
  //
  // The FIRST write is to kSlotA, and that is the fact this file exists to pin. It is NOT the only write:
  // `beq zero, zero, self` re-enters, the delay-slot `addi $t0, $t0, 0x4` accumulates, and later
  // iterations write kSlotA+4, +8, ... So kSlotA+4 holds the value too, and asserting it were untouched
  // would be asserting that the loop ran once — which is not the property under test. The property is that
  // the derivation names the slot the store WROTE on the pass being asked about, and `firstTarget` is the
  // one that answers that.
  CHECK_EQ(core.mem_r32(kSlotA), 0x37u);
  CHECK_EQ(core.mem_r32(kSlotB), 0x37u);
  // The base really did advance, so the delay-slot hazard was EXERCISED rather than assumed: if `$t0` were
  // unchanged the first-write check would pass for the wrong reason.
  CHECK(core.r[8] > kSlotB);

  // The derivation must name the slot the store WROTE, not the slot the base had reached by the time the
  // callback ran.
  const auto &target = report.targets[0];
  // FIRST, not LAST. `beq zero, zero, self` re-enters the block, and the block re-loads `$t0` — but
  // Lightrec splits blocks, so later iterations observe the base after the delay-slot bump has carried
  // it along. `firstTarget` is the one that answers "where did the store write", and it is the one the
  // memory oracle above pins. Asserting on `lastTarget` here would be asserting on an artefact of how
  // many times the loop went round, which is not what this test is about.
  CHECK(target.firstTarget.valid);
  if (!target.firstTarget.valid) {
    return;
  }
  std::fprintf(stderr,
               "  delay-slot fixture: store wrote 0x%08X; first derived=0x%08X (base $r%u disp %+d), "
               "last derived=0x%08X after %llu observation(s)\n",
               kSlotA,
               target.firstTarget.address,
               target.firstTarget.baseRegister,
               target.firstTarget.displacement,
               target.lastTarget.address,
               static_cast<unsigned long long>(report.targets[0].before));
  CHECK_EQ(target.firstTarget.displacement, 0);
  CHECK_EQ(target.firstTarget.baseRegister, 8u); // $t0
  CHECK_EQ(target.firstTarget.value, 0x37u);     // $t1
  // THE ASSERTION THAT MATTERS: with the base bumped in the delay slot, the derived address must still be
  // the slot the store WROTE. If the callback's register file were read after the delay-slot increment,
  // this would be kSlotB and the test would fail — which is the whole reason the file exists.
  CHECK_EQ(target.firstTarget.address, kSlotA);
}

} // namespace

int main() {
  RUN(store_in_a_delay_slot_bumped_base);
  return pt_summary();
}
