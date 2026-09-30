// side_effect_journal — the ordered log of everything a guest call does OUTSIDE guest RAM, for the
// per-function override differential (`override_differential.h`).
//
// WHY IT EXISTS. A RAM snapshot cannot see a GP0 write, a CD command, a timer read, a BIOS call or a
// syscall, and running a function twice — once as the original, once as the native override — would
// apply every one of those to the real devices TWICE. So the differential journals the first path
// (the original, run live, `Mode::Record`) and REPLAYS it to the second (the override,
// `Mode::Replay`): device reads are served from the recorded log by position and never reach a device,
// device writes are recorded and swallowed, and anything that cannot be replayed is withheld. The run
// then continues from the original's state, so the devices only ever saw one execution.
//
// WHAT IS JOURNALED, AND ONLY THIS. Everything guest code or native code routes through `Core`'s own
// device funnel (`Core::mem_r*`/`mem_w*` on an address with no RAM mapping, which is where every
// translated Lightrec device access also lands), every host-service dispatch from guest code
// (`dispatchGuestHostService`: title overrides, platform HLE leaves, BIOS tables, the pad work area),
// syscalls, and pending-work servicing. A native override that calls a device model DIRECTLY in C++,
// bypassing `Core`'s memory API, is invisible here — the report says so rather than implying otherwise.
//
// Core holds a NON-OWNING pointer to the active journal (`Core::sideEffectJournal`), set only for the
// dynamic extent of one differential path by `SideEffectJournal::Scope`. Null is the product state and
// costs one predicted-false branch on the device path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Core;

namespace psx::cpu {

// THE BOUND ON ONE CALL'S JOURNAL. MEASURED-SIZED, and the measurement is stated here rather than
// left to the reader's imagination — docs/issues/0141.
//
// WHY A BOUND AT ALL. The journal is the differential's memory, and the differential exists to catch
// a WRONG native override, including one that never returns. Measured in 0141: a candidate override
// for Spyro 1 `update_player_frame` (0x8003FE40) looped inside one call, storing to a device address
// each pass, and `effects_` — an unbounded `std::vector` — grew at ~6 MB/s under `PSXPORT_OVERRIDE_DIFF`
// to 7.4 GB of peak address space, `std::bad_alloc` in `append` included. Eight such gates explain
// every memory reap of 2026-09-29/30. A diagnostic that turns the worst kind of wrong override into a
// host OOM is not a gate.
//
// WHY NOT SMALLER, WITH THE MEASUREMENT. The bound has to sit far above the largest REAL call, and a
// PSX function's journaled traffic is data movement rather than waiting: the long waits (VSync,
// CdReadSync, MDEC synchronisation, wait) are `platform_hle` services, which the journal marks
// UNREPLAYABLE instead of counting, so they never reach a count at all.
//
// MEASURED 2026-09-30 over the differential's own per-call log line ("N side effect(s) replayed"),
// kept in the port repositories' run artifacts under `scratch/`, across FIVE TITLES and every override
// they have gated so far: 1,011 real shadowed calls — 6,574 completed differential reports covering
// 166,098 sampled calls over 132 distinct real override names — of which the per-call effect count was
// recorded for 1,011. The distribution is `p50 = 0`, `p99 = 94`, `max = 145` (the largest is Spyro 1's
// `update_active_voices @0x8005637C`; the largest count any real report names as a log LENGTH is 1, and
// the largest side-effect INDEX a real mismatch names is #12, which is a lower bound of 13). The
// framework's own hermetic corpus adds 18 sampled calls, max 4,097 — that is
// `tests/test_override_differential.cpp`'s deliberate many-effect case, a 4096-pass I_STAT-ack loop, and
// it is the largest effect count this repository produces on purpose.
//
// So the physical headroom is stated three ways: 1,048,576 is 256x the largest effect count measured
// in ANY real call in this workspace (145), 7,223x the framework's own largest deliberate case, and 2x
// the largest movement one function can physically make — the machine's whole 2 MiB of VRAM in 4-byte
// transfers, 524,288 words. A PSX function cannot journal more than it moves, so that last figure is a
// ceiling no real override can pass, not an estimate.
//
// The claim is re-checkable rather than a comment nobody can test: `OverrideDifferential`'s per-key
// summary prints the largest sampled call against this number on every real run, and the JSON report
// carries `largest_sampled_call_effects` beside `effects_bound`.
//
// WHAT THE BOUND BUYS. `SideEffect` is 16 bytes, so one journal is capped at 16 MiB and one shadowed
// call — which holds the live journal and the shadow journal at once — at 32 MiB, in place of the
// unbounded growth above. Reaching it is a FAILURE, never a wrap and never a silent truncation: see
// `JournalBoundViolation` and `boundViolation()`.
inline constexpr std::size_t kMaxSideEffectsPerCall = 1u << 20;

enum class SideEffectKind : std::uint8_t {
  DeviceRead,
  DeviceWrite,
  NestedOverride, // a title override the path reached by `jal`; executed on both paths
  PlatformService,
  BiosCall,
  PadWorkArea,
  Syscall,
  PendingWork,
};

// WHICH PATH A JOURNAL LOGS. Namespace scope rather than nested in `SideEffectJournal` because
// `JournalBoundViolation` names it, and a violation must be describable before the journal is.
enum class SideEffectMode : std::uint8_t {
  Record, // the live path: accesses reach the devices and are logged
  Replay, // the shadow path: accesses are logged and served from, or checked against, `recorded`
};

const char *sideEffectKindName(SideEffectKind kind);

struct SideEffect {
  SideEffectKind kind = SideEffectKind::DeviceRead;
  std::uint32_t address = 0; // device address, or the guest entry of a host service
  std::uint32_t width = 0;   // bytes, for device accesses
  std::uint32_t value = 0;   // the value read or written; the BIOS function or syscall code otherwise

  friend constexpr bool operator==(const SideEffect &, const SideEffect &) = default;
};

std::string describeSideEffect(const SideEffect &effect);

// ONE CALL PRODUCED MORE EFFECTS THAN `kMaxSideEffectsPerCall`. This is the journal's only fault, and
// it is recorded rather than applied: `effects_` stops growing, the refusal is COUNTED, and the
// differential turns the record into a verdict. A truncated log compared against a whole one is the
// silent-skip failure docs/issues/0141 forbids, so nothing here drops an entry quietly.
struct JournalBoundViolation {
  SideEffectMode mode = SideEffectMode::Record;
  // The effect that reached the bound, so the report can name the device address and width rather than
  // only a count.
  SideEffect offending{};
  std::size_t effects = 0; // journaled when the bound was reached; equals the bound
  std::size_t refused = 0; // effects refused after it, counted rather than dropped silently
  // Whether the host body was stopped by the raise. False on the live path and anywhere a translated
  // frame is on the stack, where a `throw` is illegal — see `SideEffectJournal::TranslatedExecutionScope`.
  bool stoppedCall = false;
  // The whole fault as one sentence: which path, which effect, how many were journaled, the bound, and
  // what happened to the call. The override's own name and address are NOT here — the journal does not
  // know them, and the differential's owner composes them in.
  std::string describe() const;
};

// Raised at the bound, and ONLY where a `throw` is legal. See `append`'s comment in the .cpp.
class JournalBoundExceeded {};

class SideEffectJournal {
public:
  // `recorded` is the live path's log and must outlive this journal; empty for Record mode.
  SideEffectJournal(SideEffectMode mode, std::span<const SideEffect> recorded);

  // Makes `journal` the Core's active journal for one scope and restores the previous one on exit.
  class Scope {
  public:
    Scope(Core &core, SideEffectJournal &journal);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Core &core_;
    SideEffectJournal *previous_ = nullptr;
  };

  // DEVICE FUNNEL. Replay serves a read from the recorded log (never reaching a device) and returns
  // it; Record returns nullopt and the caller performs the real read, then calls `recordDeviceRead`.
  std::optional<std::uint32_t> replayDeviceRead(std::uint32_t address, std::uint32_t width);
  void recordDeviceRead(std::uint32_t address, std::uint32_t width, std::uint32_t value);
  // Logs the write. Returns true when the write must reach the device (Record), false when it is
  // swallowed (Replay).
  bool admitDeviceWrite(std::uint32_t address, std::uint32_t width, std::uint32_t value);

  // HOST SERVICES. Returns true when the service may execute. A nested title override executes on both
  // paths (its device traffic goes through this same funnel); every other service is unreplayable:
  // Record lets it run live and marks the call incomparable, Replay withholds it and marks the same.
  bool admitHostService(SideEffectKind kind, std::uint32_t guestAddress, std::uint32_t selector);
  bool admitSyscall(std::uint32_t code, std::uint32_t instructionPc);
  // Pending work is asynchronous to the call. Record services it (and the call becomes incomparable,
  // because an interrupt or host turn ran inside it); Replay withholds it, leaving it pending for the
  // continued live state, and does not count it against the shadow path.
  bool admitPendingWork();
  // Guest time is advanced once, by the live path. The shadow path runs with the clock held.
  bool withholdsGuestTime() const;

  SideEffectMode mode() const;
  std::span<const SideEffect> effects() const;
  // The first reason this call cannot be compared, or nullopt.
  const std::optional<std::string> &unreplayable() const;
  // Replay only: the index of the first effect that differs from `recorded` (including a length
  // difference), or nullopt when the two ordered logs are equal.
  std::optional<std::size_t> firstDivergence() const;
  std::uint64_t withheldPendingWork() const;
  // Set the first time an effect is refused at `kMaxSideEffectsPerCall`, and never cleared. A journal
  // holding one has a TRUNCATED log, so the differential must record a failure rather than compare it.
  const std::optional<JournalBoundViolation> &boundViolation() const;

  // THE UNWIND GUARD, and the reason it exists.
  //
  // A `throw` from a device access is only legal while every frame between here and the shadow
  // boundary is HOST code, and the journal cannot see that from inside itself: a translated Lightrec
  // frame sits directly below the device callback (`Core::mem_w*` is reached from generated code), so
  // "am I in host code" is a fact about the CALLER, not about this frame. The executor is the only
  // thing that knows, so it marks the one place translated code runs — `executeWithBoundary`, which
  // `execute`/`executeUntilExit`/`executeFunction` all funnel through — and the journal then refuses to
  // raise while the mark is set. It records the violation either way, so the fault is reported whether
  // or not the call could be stopped.
  //
  // This is also why the LIVE path never unwinds at all: the original's device traffic comes from
  // translated guest code, so a `throw` there would cross JIT frames on every overrun rather than only
  // on the rare one. The live path's growth is already bounded by the executor's cycle budget, so it
  // terminates through the ordinary bounded-exit path and the violation is reported when it does.
  class TranslatedExecutionScope {
  public:
    explicit TranslatedExecutionScope(Core &core);
    ~TranslatedExecutionScope();
    TranslatedExecutionScope(const TranslatedExecutionScope &) = delete;
    TranslatedExecutionScope &operator=(const TranslatedExecutionScope &) = delete;

  private:
    SideEffectJournal *journal_ = nullptr;
  };

private:
  void markUnreplayable(std::string reason);
  // Refuses the effect and records the fault when the call is already at `kMaxSideEffectsPerCall`. The
  // entry is NOT stored, so a refused effect can never shift the positions the replay compares by.
  void append(SideEffect effect);
  void refuseAtBound(SideEffect effect);
  bool matchesRecordedAt(std::size_t index, const SideEffect &effect) const;

  SideEffectMode mode_;
  std::span<const SideEffect> recorded_;
  std::vector<SideEffect> effects_;
  std::optional<std::string> unreplayable_;
  std::optional<std::size_t> firstDivergence_;
  std::uint64_t withheldPendingWork_ = 0;
  std::optional<JournalBoundViolation> boundViolation_;
  // How many `TranslatedExecutionScope`s are open. Only a NATIVE body is raiseable, because only it is
  // host code all the way down.
  std::uint32_t translatedExecutions_ = 0;
};

} // namespace psx::cpu
