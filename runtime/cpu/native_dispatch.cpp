#include "native_dispatch.h"

#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "lightrec_executor.h"
#include "platform_hle.h"

#include <algorithm>
#include <cstdlib>
#include <lucent/log.h>

namespace psx::cpu {

class SuppressionScope {
public:
  SuppressionScope(NativeDispatcher &dispatcher, NativeKey key) : dispatcher_(dispatcher) {
    dispatcher_.pushSuppression(key);
  }
  ~SuppressionScope() {
    dispatcher_.popSuppression();
  }

private:
  NativeDispatcher &dispatcher_;
};

NativeDispatcher::NativeDispatcher(Core &core) : core_(core) {}

std::size_t NativeDispatcher::NativeKeyHash::operator()(NativeKey key) const {
  const std::size_t a = static_cast<std::size_t>(key.image.id ^ (key.image.id >> 32));
  const std::size_t g = static_cast<std::size_t>(key.image.generation ^ (key.image.generation >> 32));
  return a ^ (g << 1) ^ (static_cast<std::size_t>(key.address) << 2);
}

bool NativeDispatcher::install(NativeRegistration registration) {
  if (!registration.key.image.id || !registration.key.image.generation || !registration.function ||
      registration.name.empty()) {
    lucent::error("native-dispatch", "refused incomplete native override registration");
    return false;
  }
  const auto [entry, inserted] =
      entries_.try_emplace(registration.key, Entry{std::string(registration.name), registration.function});
  if (!inserted) {
    lucent::error("native-dispatch",
                  "refused duplicate override '{}' at image {}:{} address 0x{:08X}; owned by '{}'",
                  registration.name,
                  registration.key.image.id,
                  registration.key.image.generation,
                  registration.key.address,
                  entry->second.name);
    return false;
  }
  core_.lightrecExecutor().invalidate(
      {registration.key.address & 0x1fffffffu, (registration.key.address & 0x1fffffffu) + 4u});
  return true;
}

bool NativeDispatcher::remove(NativeKey key) {
  if (!entries_.erase(key)) {
    return false;
  }
  core_.lightrecExecutor().invalidate({key.address & 0x1fffffffu, (key.address & 0x1fffffffu) + 4u});
  return true;
}

bool NativeDispatcher::suppressed(NativeKey key) const {
  return std::find(suppressions_.begin(), suppressions_.end(), key) != suppressions_.end();
}

void NativeDispatcher::pushSuppression(NativeKey key) {
  suppressions_.push_back(key);
}

void NativeDispatcher::popSuppression() {
  suppressions_.pop_back();
}

bool NativeDispatcher::isInstalled(NativeKey key) const {
  return entries_.find(key) != entries_.end();
}

bool NativeDispatcher::intercepts(NativeKey key) const {
  return isInstalled(key) && !suppressed(key);
}

namespace {

class NativeExecutionScope {
public:
  NativeExecutionScope(Core &core, std::uint32_t guestAddress)
      : core_(core), previousPc_(core.pc), previousActiveAddress_(core.active_native_address),
        continuation_(core.r[31]) {
    core_.active_native_address = guestAddress;
    core_.pc = guestAddress;
  }

  ~NativeExecutionScope() {
    core_.active_native_address = previousActiveAddress_;
    if (previousActiveAddress_ != 0) {
      core_.pc = previousPc_;
    }
  }

  void completeReturn() {
    core_.pc = continuation_;
  }

  // WHERE THE GUEST CONTINUES after this leaf, which is `r[31]` — the address the guest's own `jal`
  // left there. It is captured at scope entry precisely because `core.pc` is the leaf's ENTRY while
  // the body runs, so the entry is never a valid resume point for anything this leaf does.
  [[nodiscard]] std::uint32_t continuation() const {
    return continuation_;
  }

private:
  Core &core_;
  std::uint32_t previousPc_ = 0;
  std::uint32_t previousActiveAddress_ = 0;
  std::uint32_t continuation_ = 0;
};

class NativeCallerContextScope {
public:
  explicit NativeCallerContextScope(Core &core)
      : core_(core), previousPc_(core.pc), restore_(core.active_native_address != 0) {}

  ~NativeCallerContextScope() {
    if (restore_) {
      core_.pc = previousPc_;
    }
  }

private:
  Core &core_;
  std::uint32_t previousPc_ = 0;
  bool restore_ = false;
};

struct ResolvedHostDispatch {
  GuestHostDispatchKind kind = GuestHostDispatchKind::ExecuteGuest;
  NativeKey nativeKey{};
  NativeFunction platformFunction = nullptr;
  std::optional<Hle::PadWorkAreaAction> padWorkAreaAction;
  char biosTable = 0;
};

// THE ORDER HERE IS THE CONTRACT, and it was backwards.
//
// `platform_hle` is the table of MEASURED HARDWARE-SERVICE LEAVES — VSync, CdReadSync, MDEC
// synchronisation, wait. It is a fallback for a service nobody has claimed, not a prior claim on an
// address. The image-scoped native override table is a TITLE's own recovered behaviour at a specific
// (image, generation, address) key, and the documented contract is that "a normal guest call resolves
// that complete key and invokes the native override when one is active" (AGENTS.md, Image-scoped
// native calls).
//
// Consulting the HLE table first therefore made a title-owned override UNREACHABLE for any address the
// HLE also claimed, silently. Measured on Mega Man X4, where the title installs its own movie VSync
// boundary at `0x800E4DB0` (`x4::movie::fieldBoundary`, via `movie::registerOverrides`) while the
// legacy `GameConfig::hle` window's `.vsyncTrap = 0x800E4DB0` also installs the framework's
// `PlatformHle::vsync` there: the framework's builtin won, it requested the bounded exit with `core.pc`
// (the VSync ENTRY) instead of `core.r[31]` (the call's return address), and so the movie task resumed at
// the VSync entry forever. The guest's STR frame pull could never execute its retry increment, `StGetNext`
// was never re-entered, the libstr ring filled to READY and was never read, and the completion
// transaction at `0x80018E50` — the only path that stops the drive and lets the gameplay prefix run —
// was never entered. The result was 200 fields, 0 prims offered, and 7 of 7 presents 100% black.
//
// So: the title's own override is consulted FIRST. Every other outcome below is unchanged, and an
// address no title overrides still resolves exactly as it did — the HLE table still answers for it,
// which is what it is for.
ResolvedHostDispatch resolveHostDispatch(Core &core, std::uint32_t guestAddress) {
  const std::uint32_t physical = guestAddress & 0x1fffffffu;
  const bool isRam = physical != 0;

  // 1. The title's own image-scoped override, before any host-service table.
  if (isRam) {
    if (const auto identity = core.currentImageIdentity(guestAddress)) {
      const NativeKey key{*identity, guestAddress};
      if (core.nativeDispatcher().intercepts(key)) {
        return {.kind = GuestHostDispatchKind::HostService, .nativeKey = key};
      }
    }
  }

  // 2. The measured host-service leaves, for every address no title has claimed.
  if (core.game) {
    if (NativeFunction service = core.game->platform_hle.lookup(guestAddress)) {
      return {.kind = GuestHostDispatchKind::HostService, .platformFunction = service};
    }
    const char biosTable = physical == 0xa0u ? 'A' : physical == 0xb0u ? 'B' : physical == 0xc0u ? 'C' : 0;
    if (biosTable) {
      return {.kind = GuestHostDispatchKind::HostService, .biosTable = biosTable};
    }
    if (auto action = core.game->hle.padWorkAreaAction(guestAddress)) {
      return {.kind = GuestHostDispatchKind::HostService, .padWorkAreaAction = action};
    }
  }
  if (!isRam) {
    return {.kind = GuestHostDispatchKind::HostService};
  }
  // 3. A RAM address in no active image is still the typed fault it was, not a guess.
  if (!core.currentImageIdentity(guestAddress)) {
    return {.kind = GuestHostDispatchKind::Fault};
  }
  return {};
}

} // namespace

ExecutionResult
invokeNativeFunction(Core &core, std::uint32_t guestAddress, NativeFunction function, std::string_view name) {
  NativeExecutionScope execution(core, guestAddress);
  function(&core);
  if (auto requested = core.executionControl().consume()) {
    // A leaf the guest reached by `jal` that asks for a bounded exit still resumes where a RETURN
    // would have resumed it, and `r[31]` is the only address that is correct for that: `core.pc` is
    // this leaf's own entry for the whole body, so an exit stamped with it resumes INSIDE the leaf it
    // just ran and spins forever. Measured 2026-09-27 on Mega Man X4: a positive-mode VSync exited
    // as `frame-boundary` carrying its own entry, so the boundary could never be crossed — 42 call
    // sites share that leaf, and the one after the movie completed was `VSync(-1)`, which made the
    // title look stalled rather than mis-resumed.
    //
    // The requester's own PC wins when it stated one, and then `core.pc` is left exactly as the body
    // left it: a leaf that resumes somewhere it chose is being resumed by its caller's own scope,
    // which restores the enclosing PC on scope exit. Measured 2026-09-27 on Spider-Man 1: a
    // `CooperativeYield` that stated the original body's entry must leave `core.pc` at that entry,
    // and completing the return here overwrote it with the leaf's r[31] instead.
    if (requested->guestPc == 0u) {
      // Nothing was stated, so this leaf's continuation IS the answer — and `core.pc` is brought
      // into agreement with it, because a consumer that reads one and not the other must not find
      // two different places.
      requested->guestPc = execution.continuation();
      execution.completeReturn();
    }
    return *requested;
  }
  execution.completeReturn();
  return {ExecutionExitReason::GuestReturn, core.pc, 0, std::string(name)};
}

GuestHostDispatchKind classifyGuestHostDispatch(Core &core, std::uint32_t guestAddress) {
  return resolveHostDispatch(core, guestAddress).kind;
}

ExecutionResult dispatchGuestHostService(Core &core, std::uint32_t guestAddress) {
  const ResolvedHostDispatch resolved = resolveHostDispatch(core, guestAddress);
  if (resolved.kind == GuestHostDispatchKind::Fault) {
    lucent::error(
        "native-dispatch", "guest address 0x{:08X} resolves to zero or multiple active code images", guestAddress);
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  if (resolved.kind != GuestHostDispatchKind::HostService) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "guest address has no host service"};
  }
  if (resolved.platformFunction) {
    return invokeNativeFunction(core, guestAddress, resolved.platformFunction, "platform-hle");
  }
  if (resolved.padWorkAreaAction) {
    NativeExecutionScope execution(core, guestAddress);
    core.game->hle.applyPadWorkAreaAction(*resolved.padWorkAreaAction);
    execution.completeReturn();
    return {ExecutionExitReason::GuestReturn, core.pc, 0, "BIOS pad work area"};
  }
  if (resolved.biosTable) {
    NativeExecutionScope execution(core, guestAddress);
    const std::uint32_t function = core.r[9] & 0xffu;
    if (core.game->hle.dispatchBios(resolved.biosTable, function)) {
      execution.completeReturn();
      return {ExecutionExitReason::GuestReturn, core.pc, 0, "BIOS HLE"};
    }
    lucent::error("native-dispatch", "unimplemented BIOS {}0:0x{:02X}", resolved.biosTable, function);
    return {ExecutionExitReason::Fault, guestAddress, 0, "unimplemented BIOS service"};
  }
  if (resolved.nativeKey.image.id != 0) {
    auto attribution = core.callAttribution.scope(guestAddress);
    if (auto result = core.nativeDispatcher().invoke(resolved.nativeKey)) {
      return *result;
    }
    return {ExecutionExitReason::Fault, guestAddress, 0, "native override disappeared during dispatch"};
  }
  core.pc = core.r[31];
  return {ExecutionExitReason::GuestReturn, core.pc, 0, "null callback"};
}

ExecutionResult dispatchGuest(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  const GuestHostDispatchKind kind = classifyGuestHostDispatch(core, guestAddress);
  if (kind != GuestHostDispatchKind::ExecuteGuest) {
    return dispatchGuestHostService(core, guestAddress);
  }
  auto attribution = core.callAttribution.scope(guestAddress);
  return core.lightrecExecutor().executeFunction(guestAddress, core.r[31], budget);
}

ExecutionResult dispatchGuestUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  auto attribution = core.callAttribution.scope(guestAddress);
  return core.lightrecExecutor().executeUntilExit(guestAddress, budget);
}

std::optional<ExecutionResult> NativeDispatcher::invoke(NativeKey key) {
  const auto entry = entries_.find(key);
  if (entry == entries_.end() || suppressed(key)) {
    return std::nullopt;
  }
  return invokeNativeFunction(core_, key.address, entry->second.function, entry->second.name);
}

void dispatchGuestToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(dispatchGuest(core, guestAddress, budget), owner)) {
    std::abort();
  }
}

namespace {

ExecutionResult executeOriginal(Core &core, NativeKey key, ExecutionBudget budget, bool stopAtReturn) {
  NativeCallerContextScope callerContext(core);
  SuppressionScope suppression(core.nativeDispatcher(), key);
  return stopAtReturn ? core.lightrecExecutor().executeFunction(key.address, core.r[31], budget)
                      : core.lightrecExecutor().executeUntilExit(key.address, budget);
}

ExecutionResult executeOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, bool stopAtReturn) {
  const auto identity = core.currentImageIdentity(guestAddress);
  if (!identity) {
    return {ExecutionExitReason::Fault, guestAddress, 0, "ambiguous code-image identity"};
  }
  return executeOriginal(core, NativeKey{*identity, guestAddress}, budget, stopAtReturn);
}

} // namespace

// ---- RESUMING AN ORIGINAL CALL THAT OUTLIVED ONE HOST TURN -----------------------------------------
// WHY THIS EXISTS. `ExecutionBudget::currentTurn` is one display field, 33'868'800/60 = 564,480
// cycles, by construction (execution_exit.cpp:9-16). The executor contract says that is FINE:
// "Budget exhaustion is an ordinary bounded exit" and "Host code commits and handles that state,
// then resumes deliberately" (AGENTS.md, Executor contract; docs/faithful-execution.md:20-23). So a
// guest function that legitimately needs more than one field is supposed to be resumed across the
// boundary, not aborted.
//
// Two titles were aborting instead, and both are finite compute rather than a spin:
//   - Mega Man X4's `DecDCTvlc` (0x800ED574) was measured terminating on its OWN after 610,746
//     cycles — 1.082 fields, 8.2% over — returning `GuestReturn` at the correct return address
//     0x80018AA0. It was killed for needing 8% more than a field.
//   - Crash Bash's MENU image channel swap at 0x80018AA0 is a bounded full-image loop whose body is
//     only `lhu`/`sh` on RAM: no I/O register, no VSync poll, no CD access, so it cannot be blocked.
//
// The primitive to resume them already existed and is correct — `executeFunction` runs from an
// arbitrary address and stops when `guestPc == returnAddress` (lightrec_executor.cpp:253, :664). What
// was missing was the thin wrapper that also re-establishes the two scopes the initial call had, and
// its absence is why three repositories had each written their own suspend/resume: tekken3's
// `BoundedCall`, crashbash's `FrameGuestCall`, and X4 about to write a third.
//
// The scopes are the whole reason this is not just `executeFunction`. `SuppressionScope` stops the
// native override at `key` from being re-entered while guest code that is INSIDE that original runs —
// without it a resumed original could re-enter its own override. `NativeCallerContextScope` keeps
// caller attribution correct across the boundary. Both must be re-established on every resume, which
// is exactly what `executeOriginal` does at the three lines above.
//
// COVERAGE. There is still no HERMETIC test for this, and the reason is worth recording rather than
// guessing at. The contract wants a case that truncates a long guest call MID-LOOP and resumes it. The
// only bound the executor consults between segments is Lightrec's HOST cycle counter
// (`lightrec_current_cycle_count`), not an instruction count, so a cycle-sized budget is
// machine-dependent: the same loop finishes inside one segment on a fast host and straddles two on a
// slow one. A loop Lightrec closes over runs to completion in one segment regardless of the budget, and
// a guest loop that calls into a host override never reaches a segment boundary, because the override
// is only consulted at one. The bounded exits a test CAN produce deterministically — `Core::PW_HOST`
// servicing, `maxHostDispatches` — all land at a point where a resume has no guest work left to
// distinguish it from a restart.
//
// The composition IS covered, at the product level, which is where it was always going to have to be.
// Mega Man X4 measured it on 2026-09-27: `DecDCTvlc` (0x800ED574) returning to 0x80018AA0 needs
// 610,746 cycles — 1.082 fields — and resumes across exactly two host turns, 1 resume in 684 completed
// guest calls, with 0 `executor:error` over 200 fields. The A/B is the part that makes it evidence
// rather than assertion: rebuilding the same source with the cap at one turn reproduces the original
// failure byte-for-byte (564,510 cycles, still at 0x800ED744, SIGABRT), and nothing else differs
// between the two binaries. `executeFunction`'s arbitrary-entry / supplied-return-address behaviour is
// covered hermetically by `test_explicit_function_continuation_is_independent_of_incoming_ra`.
//
// One property the product case taught, which is easy to get wrong and is NOT enforced here: the
// return address must be captured BEFORE the first dispatch. A resume must not adopt the nested `$ra`
// the guest left behind, which is a different address and would end the call in the wrong place.
ExecutionResult
resumeOriginal(Core &core, NativeKey key, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  SuppressionScope suppression(core.nativeDispatcher(), key);
  // The key already carries the call's entry, so attribution can be scoped EXACTLY as a fresh original
  // call scopes it — which is why this entry needs no second form.
  auto attribution = core.callAttribution.scope(key.address);
  return core.lightrecExecutor().executeFunction(resumePc, returnPc, budget);
}

ExecutionResult resumeGuestToReturnFrom(
    Core &core, std::uint32_t entry, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget) {
  NativeCallerContextScope callerContext(core);
  // A FRESH dispatch scopes call attribution on the entry (`dispatchGuest` does exactly this), so a
  // resume that omits it reports a submission against the wrong frame. `ot_attr.cpp` reads
  // `callAttribution.top()` / `caller()` / `visibleDepth()` for the OT submission attribution report, so
  // this is a reporting defect, not a cosmetic one: primitives submitted during a resumed turn would be
  // attributed to the enclosing frame rather than the call being resumed. Tekken 3 hand-rolled this scope
  // because the entry-less form could not express it.
  auto attribution = core.callAttribution.scope(entry);
  return core.lightrecExecutor().executeFunction(resumePc, returnPc, budget);
}

ExecutionResult
resumeGuestToReturn(Core &core, std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget budget) {
  // WITHOUT the entry, call attribution cannot be scoped correctly: the resume point is MID-function, so
  // it is not the call this turn belongs to. This form therefore scopes on the resume point, which is the
  // best available and is NOT what a fresh dispatch would produce. A caller that reports OT submission
  // attribution across a resume must use `resumeGuestToReturnFrom` with the call's ENTRY.
  NativeCallerContextScope callerContext(core);
  auto attribution = core.callAttribution.scope(resumePc);
  return core.lightrecExecutor().executeFunction(resumePc, returnPc, budget);
}

ExecutionResult callOriginal(Core &core, NativeKey key, ExecutionBudget budget) {
  return executeOriginal(core, key, budget, true);
}

ExecutionResult callOriginal(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeOriginal(core, guestAddress, budget, true);
}

ExecutionResult callOriginalUntilExit(Core &core, NativeKey key, ExecutionBudget budget) {
  return executeOriginal(core, key, budget, false);
}

ExecutionResult callOriginalUntilExit(Core &core, std::uint32_t guestAddress, ExecutionBudget budget) {
  return executeOriginal(core, guestAddress, budget, false);
}

void callOriginalToReturn(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(callOriginal(core, key, budget), owner)) {
    std::abort();
  }
}

void callOriginalToReturn(Core &core, std::uint32_t guestAddress, ExecutionBudget budget, std::string_view owner) {
  if (!requireGuestReturn(callOriginal(core, guestAddress, budget), owner)) {
    std::abort();
  }
}

namespace {

// The bounded resume loop, shared by both key and address forms. `enter` performs the FIRST call and
// `resume` every later one, so this holds one copy of the bound, the return-address capture and the
// refusal rather than one per overload.
//
// `returnPc` is read from `$r[31]` BEFORE `enter` runs, and that ordering is load-bearing: the guest
// body will set `$r[31` as it calls deeper, so reading it afterwards would resume into a nested return
// address. This is the one thing every hand-rolled copy of this loop has to get right, which is exactly
// why it is written once. It is handed to `resume` rather than closed over because a lambda capturing
// it by reference would read the *current* `$r[31]` at resume time — the very bug the capture prevents.
template <typename Enter, typename Resume>
void callOriginalResuming(Core &core, ExecutionBudget budget, std::string_view owner, Enter enter, Resume resume) {
  const std::uint32_t returnPc = core.r[31];
  std::uint64_t cycles = 0;
  std::uint32_t turns = 0;
  ExecutionResult result = enter();
  cycles += result.cycles;
  while (!result.returned()) {
    // A budget exit is the ONE reason this function exists. Anything else is a real failure and keeps
    // `requireGuestReturn`'s existing reporting, so this does not become a second error vocabulary.
    if (result.reason != ExecutionExitReason::BudgetExhausted) {
      (void)requireGuestReturn(result, owner);
      std::abort();
    }
    if (turns >= kMaxResumedHostTurns) {
      // Refused, not spun on. The bound is NAMED, and so is the turn count reached, so this line can be
      // read as the measurement it is rather than as a hang.
      lucent::error("executor",
                    "{}: the guest call to return address 0x{:08X} consumed {} host turn(s) and {} cycles "
                    "without returning and is still at 0x{:08X}. A guest function needing more than {} "
                    "display fields does not exist, so this is a guest loop: reported, not spun on",
                    owner,
                    returnPc,
                    turns,
                    cycles,
                    result.guestPc,
                    kMaxResumedHostTurns);
      std::abort();
    }
    // Each turn gets the CURRENT turn's budget, not the one the caller passed. The caller's budget
    // sized the first turn; reusing it would silently give every later turn the original allowance.
    result = resume(result.guestPc, returnPc, ExecutionBudget::currentTurn(core));
    cycles += result.cycles;
    ++turns;
  }
}

} // namespace

void callOriginalToReturnResuming(Core &core, NativeKey key, ExecutionBudget budget, std::string_view owner) {
  callOriginalResuming(
      core,
      budget,
      owner,
      [&] {
        return callOriginal(core, key, budget);
      },
      [&](std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget turn) {
        return resumeOriginal(core, key, resumePc, returnPc, turn);
      });
}

void callOriginalToReturnResuming(Core &core,
                                  std::uint32_t guestAddress,
                                  ExecutionBudget budget,
                                  std::string_view owner) {
  // The address form resumes through the ENTRY-based seam, which is the framework's own answer for a
  // caller that has an address rather than a `NativeKey`. It takes the entry explicitly, so OT
  // submission attribution during a resumed turn stays scoped to this call instead of adopting the
  // frame that encloses it — the reason that second entry point exists at all.
  callOriginalResuming(
      core,
      budget,
      owner,
      [&] {
        return callOriginal(core, guestAddress, budget);
      },
      [&](std::uint32_t resumePc, std::uint32_t returnPc, ExecutionBudget turn) {
        return resumeGuestToReturnFrom(core, guestAddress, resumePc, returnPc, turn);
      });
}

} // namespace psx::cpu
