#include "guest_redraw.h"

#include "emulated_time.h"
#include "execution_control.h"
#include "execution_exit.h"
#include "native_dispatch.h"

#include <lucent/log.h>

#include <algorithm>

namespace psx::present {

GuestRedraw::GuestRedraw(Core &core)
    : core_(core), gte_(core.gte()), hi_(core.hi), lo_(core.lo), pc_(core.pc), journalScope_(core, journal_) {
  std::copy_n(core.r, gpr_.size(), gpr_.begin());
  std::copy_n(core.cop0, cop0_.size(), cop0_.begin());
  core_.writeRollback.arm();
}

GuestRedraw::~GuestRedraw() {
  core_.writeRollback.rollBack();
  std::copy(gpr_.begin(), gpr_.end(), core_.r);
  std::copy(cop0_.begin(), cop0_.end(), core_.cop0);
  core_.hi = hi_;
  core_.lo = lo_;
  core_.pc = pc_;
}

void GuestRedraw::writeWord(std::uint32_t address, std::uint32_t word) {
  core_.mem_w32(address, word);
}

bool GuestRedraw::run(std::uint32_t entry, std::span<const std::uint32_t> arguments) {
  std::copy(arguments.begin(), arguments.end(), core_.r + cpu::kA0);
  // No call inside it can suspend at a turn's end; a second of guest time only stops a runaway.
  const cpu::ExecutionBudget runaway = cpu::ExecutionBudget::fromCycles(frame::kNominalPsxCpuHz);
  const cpu::ExecutionControl::Unsuspendable unsuspendable(core_.executionControl(), runaway);
  const cpu::ExecutionResult result = cpu::callOriginal(core_, entry, runaway);
  if (!result.returned()) {
    lucent::warn("redraw",
                 "the redraw of 0x{:08X} stopped: {} at 0x{:08X}",
                 entry,
                 cpu::executionExitName(result.reason),
                 result.guestPc);
    return false;
  }
  const auto touched = std::ranges::find_if(journal_.effects(), [](const cpu::SideEffect &effect) {
    return effect.kind == cpu::SideEffectKind::DeviceRead || effect.kind == cpu::SideEffectKind::DeviceWrite ||
           effect.kind == cpu::SideEffectKind::Syscall;
  });
  if (touched != journal_.effects().end()) {
    lucent::warn("redraw", "the redraw of 0x{:08X} reached {}", entry, cpu::describeSideEffect(*touched));
    return false;
  }
  return true;
}

} // namespace psx::present
