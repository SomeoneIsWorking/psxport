// test_resumable_guest_call.cpp — one guest call that crosses display fields: the boundary is the
// caller's, the loop is bounded, and both refusals are answers rather than a hang.
//
// The failure this covers is a class of silent corruption, not a crash. If a resume adopted the
// nested `$ra` the guest body left behind, the call would end at the wrong place and the run would
// continue; if the turn cap were missing, a guest that never returns would hang the product. Both are
// asserted here against a real dynarec execution of a synthetic guest image.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "resumable_guest_call.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

namespace {

constexpr std::uint32_t rZero = 0, rV0 = 2, rS0 = 16, rRa = 31;

// The outer entry and the return address its caller stands at. The body's nested call links `$ra` to
// an address INSIDE this image, which is the value a resume would wrongly adopt if the boundary were
// re-read per segment.
constexpr std::uint32_t kEntry = 0x00010000u;
constexpr std::uint32_t kOuterReturn = 0x00030000u;
constexpr std::uint32_t kNestedReturn = kEntry + 12u;
constexpr std::uint32_t kSpinEntry = 0x00020000u;

constexpr std::uint32_t lui(std::uint32_t rt, std::uint32_t imm) {
  return (0x0Fu << 26) | (rt << 16) | (imm & 0xFFFFu);
}
constexpr std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::int32_t imm) {
  return (0x09u << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFFu);
}
constexpr std::uint32_t bne(std::uint32_t rs, std::uint32_t rt, std::int32_t wordOffset) {
  return (0x05u << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(wordOffset) & 0xFFFFu);
}
constexpr std::uint32_t jr(std::uint32_t rs) {
  return (0x08u) | (rs << 21);
}
constexpr std::uint32_t jal(std::uint32_t target) {
  // `jal` encodes the target as a word index, and links `$ra` to PC + 8 because of the delay slot.
  return (0x03u << 26) | ((target >> 2u) & 0x03FFFFFFu);
}
constexpr std::uint32_t addu(std::uint32_t rd, std::uint32_t rs, std::uint32_t rt) {
  // SPECIAL: opcode 0, funct in bits 5:0.
  return (rs << 21) | (rt << 16) | (rd << 11) | 0x21u;
}
constexpr std::uint32_t nop() {
  return 0;
}

// word 0: save the caller's boundary, run the inner loop, put the boundary back, then return.
// The inner loop counts 100 iterations in `$v0` and `$a0`, so a small first turn MUST suspend.
constexpr std::size_t kProgramWords = 8u + 8u;
constexpr std::array<std::uint32_t, kProgramWords> kProgram{
    addu(rS0, rRa, rZero), // s0 = the boundary this call latched
    jal(kEntry + 32u),     // inner
    nop(),
    addu(rRa, rS0, rZero), // the body disturbs $ra and restores it: a per-segment re-read
    addiu(rV0, rV0, 7),    // of $ra would coincidentally agree on the LAST segment
    jr(rRa),
    nop(),
    nop(),
    // inner: a0 = 100, v0 = 0, then v0 += 1 / a0 -= 1 / bne
    addiu(4, rZero, 100),
    addiu(rV0, rZero, 0),
    addiu(rV0, rV0, 1),
    addiu(4, 4, -1),
    bne(4, rZero, -3),
    nop(),
    jr(rRa),
    nop(),
};

constexpr std::uint32_t kProgramEnd = kEntry + 4u * static_cast<std::uint32_t>(kProgramWords);
constexpr std::uint32_t kImageEnd = kProgramEnd;

// A guest loop whose exit depends on a register, split at the branch so the host turn's budget is
// consulted: a call that never returns must be stopped by the CAP, not by a runaway block.
constexpr std::size_t kSpinWords = 4u;
constexpr std::array<std::uint32_t, kSpinWords> kSpinProgram{
    addiu(4, rZero, 1),
    addiu(rV0, rV0, 1),
    bne(4, rZero, -2),
    nop(),
};

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void registerOverrides(Game &) override {}
};

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;

  Fixture() {
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    Core &core = game->core;
    core.imageCatalog().activate("resumable-call-test", {kEntry, kProgramEnd}, 0x52455331u);
    for (std::size_t i = 0; i < kProgram.size(); ++i) {
      core.mem_w32(kEntry + static_cast<std::uint32_t>(i) * 4u, kProgram[i]);
    }
    core.imageCatalog().activate("resumable-spin-test", {kSpinEntry, kSpinEntry + 16u}, 0x53504E31u);
    for (std::size_t i = 0; i < kSpinProgram.size(); ++i) {
      core.mem_w32(kSpinEntry + static_cast<std::uint32_t>(i) * 4u, kSpinProgram[i]);
    }
    core.mem_w32(kOuterReturn + 0u, jr(rRa));
    core.mem_w32(kOuterReturn + 4u, nop());
    core.r[31] = kOuterReturn;
    core.r[2] = 0u;
  }
};

static void test_a_call_that_outlives_its_turn_is_resumed_to_the_callers_boundary(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::ResumableGuestCall call;
  call.begin(core, "test resumable call", kEntry, kOuterReturn, 64u);
  // The boundary is latched into the link register before the first dispatch.
  CHECK_EQ(core.r[31], kOuterReturn);

  // A deliberately small segment budget, so the suspension is BY CONSTRUCTION rather than a hope
  // that the host's field budget happens to be small. Later segments in a real run get one display
  // field each; the framework uses whatever budget the caller names.
  const psx::cpu::ExecutionBudget segment = psx::cpu::ExecutionBudget::fromCycles(200);
  psx::cpu::CallStep step = call.advance(std::nullopt, segment);
  CHECK(step.outcome == psx::cpu::CallOutcome::Suspended);
  // The nested `jal` linked `$ra` inside the image, so a per-segment re-read would end the call
  // there. Asserting the register proves the hazard is live in this fixture, not merely unlikely.
  CHECK_EQ(core.r[31], kNestedReturn);
  std::uint32_t resumes = 1u;
  while (step.outcome == psx::cpu::CallOutcome::Suspended && resumes < 64u) {
    step = call.advance(std::nullopt, segment);
    ++resumes;
  }
  CHECK(step.outcome == psx::cpu::CallOutcome::Returned);
  CHECK(!call.pending());
  CHECK(resumes > 1u);
  // The inner loop counted 100 and the body added 7.
  CHECK_EQ(step.value, 107u);
  // It ended where the CALLER was standing, not at the nested link address.
  CHECK_EQ(core.r[31], kOuterReturn);
  CHECK_EQ(core.pc, kOuterReturn);
  // The run's census counted this call as one that needed a resume.
  CHECK_EQ(core.guestCallCensus().completed(), 1u);
  CHECK_EQ(core.guestCallCensus().resumed(), 1u);
  CHECK(core.guestCallCensus().deepestTurns() > 1u);
}

// The cap is a policy constant in display fields, and it REFUSES by answering rather than by
// aborting, so a caller that owns the surrounding decision can report it.
static void test_the_turn_cap_refuses_a_guest_that_never_returns(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::ResumableGuestCall call;
  call.begin(core, "test capped call", kSpinEntry, kOuterReturn, 2u);
  const psx::cpu::CallStep first = call.advance(std::nullopt, psx::cpu::ExecutionBudget::fromCycles(64));
  CHECK(first.outcome == psx::cpu::CallOutcome::Suspended);
  const psx::cpu::CallStep second = call.advance(std::nullopt, psx::cpu::ExecutionBudget::fromCycles(64));
  CHECK(second.outcome == psx::cpu::CallOutcome::Suspended);
  const psx::cpu::CallStep third = call.advance(std::nullopt, psx::cpu::ExecutionBudget::fromCycles(64));
  CHECK(third.outcome == psx::cpu::CallOutcome::Refused);
  CHECK(third.detail.find("turn cap") != std::string::npos);
}

static void test_resuming_a_call_that_is_not_pending_is_refused(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::ResumableGuestCall call;
  const psx::cpu::CallStep step = call.advance();
  CHECK(step.outcome == psx::cpu::CallOutcome::Refused);
  CHECK(step.detail.find("not pending") != std::string::npos);
}

static void nativeOverride(Core *) {}

static void test_installing_an_override_resolves_the_image_and_refuses_a_foreign_address(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  const std::optional<psx::cpu::NativeKey> installed =
      psx::cpu::tryInstallNativeOverride(core, kEntry, "test::override", &nativeOverride);
  CHECK(installed.has_value());
  CHECK(core.nativeDispatcher().isInstalled(*installed));
  // A second owner of the same key is refused, and the refusal names the override.
  CHECK(!psx::cpu::tryInstallNativeOverride(core, kEntry, "test::second", &nativeOverride).has_value());
  // An address no active image owns cannot be attributed to a module, so it is refused.
  CHECK(!psx::cpu::tryInstallNativeOverride(core, 0x00090000u, "test::foreign", &nativeOverride).has_value());
  // So is a registration with no native body.
  CHECK(!psx::cpu::tryInstallNativeOverride(core, kEntry + 4u, "test::empty", nullptr).has_value());
}

} // namespace

int main() {
  RUN(a_call_that_outlives_its_turn_is_resumed_to_the_callers_boundary);
  RUN(the_turn_cap_refuses_a_guest_that_never_returns);
  RUN(resuming_a_call_that_is_not_pending_is_refused);
  RUN(installing_an_override_resolves_the_image_and_refuses_a_foreign_address);
  return pt_summary();
}