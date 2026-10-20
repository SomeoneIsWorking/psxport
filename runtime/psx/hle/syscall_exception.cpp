#include "syscall_exception.h"

#include "r3000.h"

namespace psx::syscall_exception {
namespace {

using psx::cpu::kCop0Cause;
using psx::cpu::kCop0Epc;
using psx::cpu::kCop0Status;
constexpr uint32_t kCauseInterruptPending = 0x0000FF00u;
constexpr uint32_t kSyscallExceptionCode = 8u;
constexpr uint32_t kCurrentModeStack = 0x3Fu;
constexpr uint32_t kReturnModeStack = 0x0Fu;
constexpr uint32_t kPreviousInterruptEnable = 1u << 2;

} // namespace

void enter(uint32_t (&cop0)[16], uint32_t syscallPc) {
  cop0[kCop0Epc] = syscallPc;
  cop0[kCop0Status] = (cop0[kCop0Status] & ~kCurrentModeStack) | ((cop0[kCop0Status] << 2) & kCurrentModeStack);
  cop0[kCop0Cause] = (cop0[kCop0Cause] & kCauseInterruptPending) | (kSyscallExceptionCode << 2);
}

void setReturnInterruptEnabled(uint32_t (&cop0)[16], bool enabled) {
  if (enabled) {
    cop0[kCop0Status] |= kPreviousInterruptEnable;
  } else {
    cop0[kCop0Status] &= ~kPreviousInterruptEnable;
  }
}

void leave(uint32_t (&cop0)[16]) {
  cop0[kCop0Status] = (cop0[kCop0Status] & ~kReturnModeStack) | ((cop0[kCop0Status] >> 2) & kReturnModeStack);
}

} // namespace psx::syscall_exception
