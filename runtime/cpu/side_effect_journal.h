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

const char *sideEffectKindName(SideEffectKind kind);

struct SideEffect {
  SideEffectKind kind = SideEffectKind::DeviceRead;
  std::uint32_t address = 0; // device address, or the guest entry of a host service
  std::uint32_t width = 0;   // bytes, for device accesses
  std::uint32_t value = 0;   // the value read or written; the BIOS function or syscall code otherwise

  friend constexpr bool operator==(const SideEffect &, const SideEffect &) = default;
};

std::string describeSideEffect(const SideEffect &effect);

class SideEffectJournal {
public:
  enum class Mode : std::uint8_t {
    Record, // the live path: accesses reach the devices and are logged
    Replay, // the shadow path: accesses are logged and served from, or checked against, `recorded`
  };

  // `recorded` is the live path's log and must outlive this journal; empty for Record mode.
  SideEffectJournal(Mode mode, std::span<const SideEffect> recorded);

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

  Mode mode() const;
  std::span<const SideEffect> effects() const;
  // The first reason this call cannot be compared, or nullopt.
  const std::optional<std::string> &unreplayable() const;
  // Replay only: the index of the first effect that differs from `recorded` (including a length
  // difference), or nullopt when the two ordered logs are equal.
  std::optional<std::size_t> firstDivergence() const;
  std::uint64_t withheldPendingWork() const;

private:
  void markUnreplayable(std::string reason);
  void append(SideEffect effect);
  bool matchesRecordedAt(std::size_t index, const SideEffect &effect) const;

  Mode mode_;
  std::span<const SideEffect> recorded_;
  std::vector<SideEffect> effects_;
  std::optional<std::string> unreplayable_;
  std::optional<std::size_t> firstDivergence_;
  std::uint64_t withheldPendingWork_ = 0;
};

} // namespace psx::cpu
