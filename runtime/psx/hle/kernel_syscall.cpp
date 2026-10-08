#include "execution_services.h"

#include "game.h"
#include "side_effect_journal.h"
#include "syscall_exception.h"

#include <cstddef>

namespace psx::cpu {
namespace {

constexpr std::size_t kV0 = 2;
constexpr std::size_t kA0 = 4;

} // namespace

SyscallResult handleSyscall(Core &core, std::uint32_t code, std::uint32_t instructionPc) {
  // The kernel selector is $a0; the instruction's code field does not select the operation.
  (void)code;
  if (!core.game) {
    return SyscallResult::MissingContext;
  }
  const auto selector = core.r[kA0];
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
    core.r[kV0] = 0;
    break;
  case 1: // EnterCriticalSection
    core.r[kV0] = interruptEnabled ? 1 : 0;
    interruptEnabled = 0;
    syscall_exception::setReturnInterruptEnabled(core.cop0, false);
    break;
  case 2: // ExitCriticalSection
    interruptEnabled = 1;
    syscall_exception::setReturnInterruptEnabled(core.cop0, true);
    core.r[kV0] = 0;
    break;
  }
  syscall_exception::leave(core.cop0);
  return SyscallResult::Handled;
}

} // namespace psx::cpu
