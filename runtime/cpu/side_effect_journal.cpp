#include "side_effect_journal.h"

#include "core.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>

namespace psx::cpu {

const char *sideEffectKindName(SideEffectKind kind) {
  switch (kind) {
  case SideEffectKind::DeviceRead:
    return "device-read";
  case SideEffectKind::DeviceWrite:
    return "device-write";
  case SideEffectKind::NestedOverride:
    return "nested-override";
  case SideEffectKind::PlatformService:
    return "platform-service";
  case SideEffectKind::BiosCall:
    return "bios-call";
  case SideEffectKind::PadWorkArea:
    return "pad-work-area";
  case SideEffectKind::Syscall:
    return "syscall";
  case SideEffectKind::PendingWork:
    return "pending-work";
  }
  return "unknown";
}

std::string describeSideEffect(const SideEffect &effect) {
  switch (effect.kind) {
  case SideEffectKind::DeviceRead:
  case SideEffectKind::DeviceWrite:
    return lucent::format("{} {}-bit @0x{:08X} = 0x{:08X}",
                          sideEffectKindName(effect.kind),
                          effect.width * 8u,
                          effect.address,
                          effect.value);
  case SideEffectKind::BiosCall:
  case SideEffectKind::Syscall:
    return lucent::format(
        "{} @0x{:08X} function 0x{:X}", sideEffectKindName(effect.kind), effect.address, effect.value);
  case SideEffectKind::NestedOverride:
  case SideEffectKind::PlatformService:
  case SideEffectKind::PadWorkArea:
  case SideEffectKind::PendingWork:
    return lucent::format("{} @0x{:08X}", sideEffectKindName(effect.kind), effect.address);
  }
  return "unknown";
}

SideEffectJournal::SideEffectJournal(Mode mode, std::span<const SideEffect> recorded)
    : mode_(mode), recorded_(recorded) {}

SideEffectJournal::Scope::Scope(Core &core, SideEffectJournal &journal)
    : core_(core), previous_(core.sideEffectJournal) {
  core_.sideEffectJournal = &journal;
}

SideEffectJournal::Scope::~Scope() {
  core_.sideEffectJournal = previous_;
}

std::optional<std::uint32_t> SideEffectJournal::replayDeviceRead(std::uint32_t address, std::uint32_t width) {
  if (mode_ != Mode::Replay) {
    return std::nullopt;
  }
  const std::size_t index = effects_.size();
  // Served from the recorded read AT THE SAME POSITION when it is the same access. After a divergence
  // the shadow path's state is already known to differ and is discarded, so the value only has to keep
  // it from reaching a device: the last value the live path read from this address, else 0.
  std::uint32_t value = 0;
  if (index < recorded_.size() && recorded_[index].kind == SideEffectKind::DeviceRead &&
      recorded_[index].address == address && recorded_[index].width == width) {
    value = recorded_[index].value;
  } else {
    for (const SideEffect &earlier : recorded_) {
      if (earlier.kind == SideEffectKind::DeviceRead && earlier.address == address && earlier.width == width) {
        value = earlier.value;
      }
    }
  }
  append({SideEffectKind::DeviceRead, address, width, value});
  return value;
}

void SideEffectJournal::recordDeviceRead(std::uint32_t address, std::uint32_t width, std::uint32_t value) {
  append({SideEffectKind::DeviceRead, address, width, value});
}

bool SideEffectJournal::admitDeviceWrite(std::uint32_t address, std::uint32_t width, std::uint32_t value) {
  append({SideEffectKind::DeviceWrite, address, width, value});
  return mode_ == Mode::Record;
}

bool SideEffectJournal::admitHostService(SideEffectKind kind, std::uint32_t guestAddress, std::uint32_t selector) {
  const SideEffect effect{kind, guestAddress, 0, selector};
  const std::size_t index = effects_.size();
  append(effect);
  if (kind == SideEffectKind::NestedOverride) {
    // Executes on the live path, and on the shadow path only where the live path did the same thing at
    // the same position: an override the original never reached must not run against live devices.
    return mode_ == Mode::Record || matchesRecordedAt(index, effect);
  }
  markUnreplayable(lucent::format(
      "{} path performed {}", mode_ == Mode::Record ? "original" : "native", describeSideEffect(effect)));
  return mode_ == Mode::Record;
}

bool SideEffectJournal::admitSyscall(std::uint32_t code, std::uint32_t instructionPc) {
  const SideEffect effect{SideEffectKind::Syscall, instructionPc, 0, code};
  append(effect);
  markUnreplayable(lucent::format(
      "{} path performed {}", mode_ == Mode::Record ? "original" : "native", describeSideEffect(effect)));
  return mode_ == Mode::Record;
}

bool SideEffectJournal::admitPendingWork() {
  if (mode_ == Mode::Replay) {
    ++withheldPendingWork_;
    return false;
  }
  append({SideEffectKind::PendingWork, 0, 0, 0});
  markUnreplayable("original path serviced asynchronous pending work (interrupt or host turn)");
  return true;
}

bool SideEffectJournal::withholdsGuestTime() const {
  return mode_ == Mode::Replay;
}

SideEffectJournal::Mode SideEffectJournal::mode() const {
  return mode_;
}

std::span<const SideEffect> SideEffectJournal::effects() const {
  return effects_;
}

const std::optional<std::string> &SideEffectJournal::unreplayable() const {
  return unreplayable_;
}

std::optional<std::size_t> SideEffectJournal::firstDivergence() const {
  if (firstDivergence_) {
    return firstDivergence_;
  }
  if (effects_.size() != recorded_.size()) {
    return std::min(effects_.size(), recorded_.size());
  }
  return std::nullopt;
}

std::uint64_t SideEffectJournal::withheldPendingWork() const {
  return withheldPendingWork_;
}

void SideEffectJournal::markUnreplayable(std::string reason) {
  if (!unreplayable_) {
    unreplayable_ = std::move(reason);
  }
}

void SideEffectJournal::append(SideEffect effect) {
  const std::size_t index = effects_.size();
  if (mode_ == Mode::Replay && !firstDivergence_ && !matchesRecordedAt(index, effect)) {
    firstDivergence_ = index;
  }
  effects_.push_back(effect);
}

bool SideEffectJournal::matchesRecordedAt(std::size_t index, const SideEffect &effect) const {
  return index < recorded_.size() && recorded_[index] == effect;
}

} // namespace psx::cpu
