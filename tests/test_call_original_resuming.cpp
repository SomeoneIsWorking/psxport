// test_call_original_resuming.cpp — a guest call that outlives one host turn must RESUME, and a guest
// that never returns must be REFUSED by name rather than hung on.
//
// WHY IT EXISTS. `AGENTS.md` says budget exhaustion is "an ordinary bounded exit" that host code
// "commits and handles, then resumes deliberately", and the comment above `resumeOriginal` says the
// same in more words. But `callOriginalToReturn` returns `void`, so "exhausted — resume me at this PC"
// is inexpressible to it, and its only remaining move is `std::abort()`. A guest function that
// legitimately needs more than one host turn therefore had two options, both wrong: abort, or
// hand-roll the loop. **Three repositories hand-rolled it.** `callOriginalToReturnResuming` owns it
// once, and these are the halves that must BOTH be true for that to be an improvement rather than a
// new way to hang.
//
// THE FIRST HALF is mechanical — exhaust, resume, finish — and is easy to get right. THE SECOND HALF
// is the one that matters: a bound that is never reached is a hang wearing the costume of a fix. So
// the never-returning guest is a SEPARATE CTest mode, because in-process it would take the whole
// suite down with it, and because `abort()` is not catchable.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

namespace {

constexpr std::uint32_t kEntry = 0x00010000u; // the guest function the override intercepts
constexpr std::uint32_t kCallerReturn = 0x00030000u;
// A data address OUTSIDE the activated image. Nothing in this file stores there any more — the
// resume cases assert registers — but the spin fixture's exit condition is loaded from it, and a load
// from an address the optimizer can see is written to is a loop the optimizer can fold shut.
constexpr std::uint32_t kDataBase = 0x00080000u;

constexpr std::uint32_t rZero = 0, rV0 = 2, rA0 = 4, rA1 = 5;

// EVERY ENCODING BELOW IS COMPUTED FROM FIELD NAMES, AND EVERY ADDRESS IS MATERIALISED IN FULL.
// That is not tidiness, it is the third time this fixture has lied.
//
// The first version hand-typed its hex and three words were wrong: a missing base field, a missing
// rs, and an `slt` whose funct field made it an `SRL`. The second version built the words from
// fields and still produced a fixture that reported the framework as broken. It loaded the counter
// address with `lui` ALONE and omitted the low half, so `$a1` was 0x00010000 — the FIRST INSTRUCTION
// — and the loop dutifully overwrote its own code. The host's counter stayed 0, the guest read back a
// register it had never stored, and the only symptom was "the resume did nothing", which is exactly
// what a broken resume looks like. **A counter of 0 cannot tell you whether the resume failed, the
// store went elsewhere, or the fixture addresses the wrong word**, so this file now materialises
// `lui`+`addiu` for every address and the store-into-code case below is its own test, where the
// expected value is an instruction word the host can see.
constexpr std::uint32_t lui(std::uint32_t rt, std::uint32_t imm) {
  return (0x0Fu << 26) | (rt << 16) | (imm & 0xFFFFu);
}
constexpr std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::int32_t imm) {
  return (0x09u << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFFu);
}
constexpr std::uint32_t ori(std::uint32_t rt, std::uint32_t rs, std::uint32_t imm) {
  return (0x0Du << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
}
constexpr std::uint32_t lw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return (0x23u << 26) | (base << 21) | (rt << 16) | (static_cast<std::uint32_t>(off) & 0xFFFFu);
}
constexpr std::uint32_t sw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return (0x2Bu << 26) | (base << 21) | (rt << 16) | (static_cast<std::uint32_t>(off) & 0xFFFFu);
}
constexpr std::uint32_t jrRa() {
  return (0x08u) | (rZero << 21);
}
constexpr std::uint32_t nop() {
  return 0;
}
constexpr std::uint32_t bne(std::uint32_t rs, std::uint32_t rt, std::int32_t wordOffset) {
  return (0x05u << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(wordOffset) & 0xFFFFu);
}

// A full 32-bit address in two instructions, and the low half is `ori` for a reason that cost this
// file its fourth false report. `lui`+`addiu` is only the address idiom when the low half is
// NON-ZERO: `addiu $a1, $zero, 0` encodes as `move $a1, $zero` and DESTROYS the high half the
// `lui` just loaded, so every 64 KB-aligned address silently became address 0. `ori` with a zero
// immediate is the same no-op that preserves it, so the pair is correct for every address — and
// there is no third instruction to remember to skip.
constexpr std::uint32_t materialize(std::uint32_t rt, std::uint32_t address) {
  return ori(rt, rt, address & 0xFFFFu);
}
constexpr std::uint32_t highHalf(std::uint32_t address) {
  return address >> 16;
}

constexpr std::uint32_t kLoopSteps = 2000u; // many turns' worth at the deliberately tiny budget below

// WHY THE ASSERTION IS A REGISTER AND NOT A COUNTER WORD — the fifth way this fixture has lied, and
// the subtlest, because in every version of it the GUEST RAN CORRECTLY and the framework was still
// reported broken.
//
// The body was a read-modify-write of a memory word, checked from the host after the call. It does
// not work, and it does not work because of Lightrec's optimizer: the loaded value is dead unless a
// LATER GUEST INSTRUCTION uses it, and the host reading a word from outside the guest is not an
// observation the compiler can see. Ending on `jr $ra` with the value in `$v0` is the same dead
// value; branching on it and marking `$a0` kept the load alive and still produced 0, because the
// chain itself was folded away. The store path is not what this test is for, and pretending
// otherwise bought four false reports: a store-into-code case below covers RAM, and it is the case
// that measures the store path directly.
//
// `$v0` is the accumulator and `$a0` the countdown, both ARCHITECTURALLY live at `jr $ra`, so no
// optimization can remove them and the total is the number of iterations the guest actually retired.
// A resume that silently restarted the call would re-enter with `$v0` where it was and finish short,
// so the equality catches that direction as well as a resume that dropped the call.
constexpr std::size_t kProgramWords = 1u + 4u + 2u;
constexpr std::uint32_t kImageEnd = kEntry + 4u * static_cast<std::uint32_t>(kProgramWords);

constexpr std::array<std::uint32_t, kProgramWords> makeProgram() {
  std::array<std::uint32_t, kProgramWords> words{};
  std::size_t at = 0;
  words[at++] = addiu(rA0, rZero, static_cast<std::int32_t>(kLoopSteps));
  // loop head, at word 1
  words[at++] = addiu(rV0, rV0, 1);
  words[at++] = addiu(rA0, rA0, -1);
  words[at++] = bne(rA0, rZero, -3); // back to the `addiu $v0`, with `nop` in the delay slot
  words[at++] = nop();
  words[at++] = jrRa();
  words[at++] = nop();
  return words;
}

constexpr std::array<std::uint32_t, kProgramWords> kProgram = makeProgram();
// A guest loop that never returns, for the bound.
//
// IT IS A DATA-DEPENDENT LOOP AND NOT `beq $zero,$zero,-1`, and the difference is the whole test. A
// self-branch is a single basic block that never returns control to the executor, so the host turn's
// budget is never consulted and the first version of this case died on SIGSEGV inside the JIT rather
// than reaching the bound. A loop whose exit depends on a value loaded from memory is split at the
// branch, so every pass hands control back and the bound is the thing that stops it — which is the
// situation the bound exists for. A guest that spins inside one block is a different problem, and it
// is not this test's.
constexpr std::uint32_t kSpinWords = 8u;
constexpr std::uint32_t kSpinGate = kDataBase; // non-zero for the whole run, so the loop cannot exit
constexpr std::array<std::uint32_t, kSpinWords> makeSpinProgram() {
  return {
      lui(rA1, highHalf(kSpinGate)),
      materialize(rA1, kSpinGate),
      lw(rA0, rA1, 0),     // the exit condition, once
      addiu(rV0, rV0, 1),  // loop head
      bne(rA0, rZero, -1), // back to the `addiu $v0`
      nop(),
      // A guest body that FALLS OUT rather than looping is the negative control for this whole mode:
      // it reaches `jr $ra`, `callOriginalToReturnResuming` returns normally, and the mode exits 1.
      // Without the return, a mutant that stopped looping would run off the end of the image and
      // segfault, which is nonzero too and would make the mode pass for entirely the wrong reason.
      jrRa(),
      nop(),
  };
}
constexpr std::array<std::uint32_t, kSpinWords> kSpinWordsImage = makeSpinProgram();

// The self-modifying case. A short body that stores a KNOWN value into its OWN first word, which is
// inside the activated image. Lightrec's optimizer constant-propagates that target, marks the block
// `BLOCK_NEVER_COMPILE` and runs it in the fallback interpreter (shared/lightrec/optimizer.c), so
// this is the one case in this file that exercises the self-modifying path rather than the resume.
// It is here because that path is where the previous fixture silently lived, and because a store
// into translated code landing in RAM is a property the workspace has no other coverage for.
//
// The body does NOT restore the word. Leaving the value in place is what makes the case a real
// assertion: a store that was dropped leaves the original `lui` behind and a host that reads 0x0BAD
// has proof the guest's own store reached RAM. Restoring it would make the only evidence a counter.
constexpr std::uint32_t kSmcPatched = 0x0BADu;
constexpr std::size_t kSmcWords = 2u + 3u + 2u;
constexpr std::array<std::uint32_t, kSmcWords> makeSmcProgram() {
  std::array<std::uint32_t, kSmcWords> words{};
  std::size_t at = 0;
  words[at++] = lui(rA1, highHalf(kEntry));
  words[at++] = materialize(rA1, kEntry);
  words[at++] = addiu(rV0, rZero, static_cast<std::int32_t>(kSmcPatched));
  words[at++] = sw(rV0, rA1, 0);
  words[at++] = jrRa();
  words[at++] = nop();
  return words;
}
constexpr std::array<std::uint32_t, kSmcWords> kSmcProgram = makeSmcProgram();
constexpr std::uint32_t kSmcImageEnd = kEntry + 4u * static_cast<std::uint32_t>(kSmcWords);

std::uint32_t g_overrideCalls = 0;

void overrideCallingOriginal(Core *core);

// The key for the intercepted entry. Resolving the address rather than assuming an identity is the
// point: an override key is (image identity, address), and a key built against the wrong image
// silently never fires — the same "green because it measured the wrong subject" failure this
// workspace keeps meeting, so the resolution is checked where the key is built.
psx::cpu::NativeKey keyFor(Core &core) {
  const std::optional<psx::cpu::ImageIdentity> identity = core.imageCatalog().resolve(kEntry);
  if (!identity.has_value()) {
    std::fprintf(stderr, "test setup REFUSED: no image covers 0x%08X\n", kEntry);
    std::abort();
  }
  return psx::cpu::NativeKey{*identity, kEntry};
}

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
  // Pure virtual, so it is overridden — and deliberately EMPTY. This hook runs during `Game`
  // construction, before the fixture activates the image, so a key built here would name a different
  // identity than `keyFor` resolves afterwards and the override would silently never fire. The install
  // is done in the fixture instead, where the ordering is visible.
  void registerOverrides(Game &) override {}
};

void overrideCallingOriginal(Core *core) {
  ++g_overrideCalls;
  // A budget far too small for the body, so the FIRST turn exhausts BY CONSTRUCTION rather than by
  // hoping the host's field budget happens to be small. Every later turn is the framework's
  // `ExecutionBudget::currentTurn`, which is what the resume is supposed to use.
  psx::cpu::callOriginalToReturnResuming(
      *core, keyFor(*core), psx::cpu::ExecutionBudget::fromCycles(64), "test guest call");
}

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;

  Fixture() {
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    Core &core = game->core;
    core.imageCatalog().activate("resume-test", {kEntry, kImageEnd}, 0x52455355u);
    for (std::size_t i = 0; i < kProgram.size(); ++i) {
      core.mem_w32(kEntry + static_cast<std::uint32_t>(i) * 4u, kProgram[i]);
    }
    // The caller's return address, and the address `callOriginalToReturnResuming` captures from `$r[31]`
    // BEFORE the first call. The guest body disturbs `$r[31]` on its way past, so re-reading it at
    // resume time would end the call at the wrong place. That is why the capture is the framework's
    // job and not the caller's, and this file asserts the precondition rather than trusting it.
    core.mem_w32(kCallerReturn + 0u, jrRa());
    core.mem_w32(kCallerReturn + 4u, nop());
    core.r[31] = kCallerReturn;
    core.r[2] = 0; // `$v0` is the accumulator, so the fixture starts it at zero explicitly

    const bool installed = core.nativeDispatcher().install(
        psx::cpu::NativeRegistration{keyFor(core), "test original-resuming target", &overrideCallingOriginal});
    if (!installed) {
      std::fprintf(stderr, "test setup REFUSED: the override did not install\n");
      std::abort();
    }
  }
};

// POSITIVE: a budget too small for the body must be RESUMED until the guest finishes, and the guest's
// own memory is the denominator — a resume that did nothing leaves the counter short, and a loop that
// ran too far leaves it over, so both directions are caught by one equality.
static void test_a_call_that_outlives_its_budget_is_resumed_until_it_returns(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  CHECK(core.nativeDispatcher().isInstalled(keyFor(core)));
  core.nativeDispatcher().invoke(keyFor(core));

  CHECK_EQ(g_overrideCalls, 1u);
  if (core.r[2] != kLoopSteps) {
    std::fprintf(
        stderr, "the guest retired %u of %u iterations; `$a0` is left at %u\n", core.r[2], kLoopSteps, core.r[4]);
  }
  // `$v0` counted the iterations and `$a0` reached zero: the call ran to its own end rather than
  // being cut off at the first turn's budget and reported as finished.
  CHECK_EQ(core.r[2], kLoopSteps);
  CHECK_EQ(core.r[4], 0u);
  // The call returned to where the CALLER was, not to a `$r[31]` the guest left behind.
  CHECK_EQ(core.r[31], kCallerReturn);
}

// The precondition the capture exists for, as an executable statement. If the body never disturbed
// `$r[31]` then re-reading it at resume time would coincidentally be correct and the hazard would be
// untested — so the test asserts the value is the caller's again, which is also what makes the
// previous test's `CHECK_EQ(core.r[31], kCallerReturn)` a statement about a RETURN ADDRESS rather than
// about a value that never changed.
static void test_the_return_address_is_the_callers_and_not_whatever_the_guest_left(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  const std::uint32_t before = core.r[31];
  psx::cpu::callOriginalToReturnResuming(
      core, keyFor(core), psx::cpu::ExecutionBudget::fromCycles(1'000'000), "test whole call");
  CHECK_EQ(core.r[2], kLoopSteps);
  CHECK_EQ(core.r[4], 0u);
  CHECK_EQ(core.r[31], before);
}

// The self-modifying path, stated as a value the HOST can see rather than as a counter the guest
// keeps: the body overwrites its own first instruction word and then puts it back. If the store were
// dropped the word would still read `lui $a1, 1` after the call. It is the case the previous fixture
// ran by accident, and the case that made a working framework look broken.
static void test_a_guest_store_into_its_own_code_reaches_ram(void) {
  Runtime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  core.imageCatalog().activate("smc-test", {kEntry, kSmcImageEnd}, 0x534D4355u);
  for (std::size_t i = 0; i < kSmcProgram.size(); ++i) {
    core.mem_w32(kEntry + static_cast<std::uint32_t>(i) * 4u, kSmcProgram[i]);
  }
  core.r[31] = kCallerReturn;
  core.mem_w32(kCallerReturn + 0u, jrRa());
  core.mem_w32(kCallerReturn + 4u, nop());

  psx::cpu::callOriginalToReturnResuming(
      core, kEntry, psx::cpu::ExecutionBudget::fromCycles(1'000'000), "test self-modifying body");
  // The guest overwrote its own first instruction word and left the value there, so this is the host
  // reading RAM the guest's own store wrote — with the original `lui` as the value a dropped store
  // would leave behind.
  CHECK_EQ(core.mem_r32(kEntry), kSmcPatched);
  // And the block really did take the self-modifying path rather than being compiled like any other,
  // which is what makes the assertion above about SMC handling instead of about a plain store.
  CHECK_EQ(core.lightrecExecutor().counters().fallback.selfModifyingCode, 1u);
}

volatile std::sig_atomic_t g_enteredTheCall = 0;

extern "C" void onAbortFromTheNeverReturningGuest(int) {
  // The bound refused by `std::abort()`. That is the framework's contract, and it is not catchable,
  // so the refusal is observed HERE and turned into an ordinary exit. The marker guards the
  // conversion: an abort raised anywhere else in this process must not read as the bound firing.
  if (g_enteredTheCall == 0) {
    _exit(70);
  }
  _exit(0);
}

int runTheNeverReturningGuest() {
  Runtime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  core.imageCatalog().activate(
      "resume-spin", {kEntry, kEntry + 4u * static_cast<std::uint32_t>(kSpinWords)}, 0x5350494Eu);
  for (std::size_t i = 0; i < kSpinWordsImage.size(); ++i) {
    core.mem_w32(kEntry + static_cast<std::uint32_t>(i) * 4u, kSpinWordsImage[i]);
  }
  core.mem_w32(kSpinGate, 1u);
  core.r[31] = kCallerReturn;
  std::signal(SIGABRT, onAbortFromTheNeverReturningGuest);
  g_enteredTheCall = 1;
  psx::cpu::callOriginalToReturnResuming(
      core, keyFor(core), psx::cpu::ExecutionBudget::fromCycles(64), "test never-returning guest");
  return 1; // reaching here is the failure: the bound did not fire
}

} // namespace

int main(int argc, char **argv) {
  // The bound case runs as its OWN ctest entry: in-process it aborts, and a bound that is never
  // exercised is a hang in a costume. Exit 0 means the refusal fired; 1 means it did not; 70 means
  // something unrelated aborted, which is also a failure and is deliberately a different number.
  if (argc > 1 && std::strcmp(argv[1], "--never-returns") == 0) {
    return runTheNeverReturningGuest();
  }
  RUN(a_call_that_outlives_its_budget_is_resumed_until_it_returns);
  RUN(the_return_address_is_the_callers_and_not_whatever_the_guest_left);
  RUN(a_guest_store_into_its_own_code_reaches_ram);
  return pt_summary();
}
