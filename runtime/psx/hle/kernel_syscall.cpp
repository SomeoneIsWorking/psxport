#include "execution_services.h"
#include "r3000.h"

#include "game.h"
#include "side_effect_journal.h"
#include "syscall_exception.h"

#include <cstddef>

namespace psx::cpu {

// The kernel selector is $a0; the instruction's code field does not select the operation.
SyscallResult handleSyscall(Core &core, std::uint32_t instructionPc) {
  if (!core.game) {
    return SyscallResult::MissingContext;
  }
  const auto selector = core.r[psx::cpu::kA0];
  // A syscall changes kernel state outside guest memory (the critical-section flag and COP0), so the
  // override differential cannot replay it: its live path runs it and becomes incomparable, and its
  // shadow path is refused it (side_effect_journal.h).
  if (core.sideEffectJournal != nullptr && !core.sideEffectJournal->admitSyscall(selector, instructionPc)) {
    return SyscallResult::Handled;
  }
  if (selector > 2u) {
    return SyscallResult::UnsupportedSelector;
  }

  syscall_exception::enter(core.cop0, instructionPc);
  int &interruptEnabled = core.game->hle.irq_enabled;
  switch (selector) {
  case 0:
    core.r[psx::cpu::kV0] = 0;
    break;
  case 1: // EnterCriticalSection
    core.r[psx::cpu::kV0] = interruptEnabled ? 1 : 0;
    interruptEnabled = 0;
    syscall_exception::setReturnInterruptEnabled(core.cop0, false);
    break;
  default: // 2, ExitCriticalSection; larger selectors were refused above
    interruptEnabled = 1;
    syscall_exception::setReturnInterruptEnabled(core.cop0, true);
    core.r[psx::cpu::kV0] = 0;
    break;
  }
  syscall_exception::leave(core.cop0);
  return SyscallResult::Handled;
}

} // namespace psx::cpu
