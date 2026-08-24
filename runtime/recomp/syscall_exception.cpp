#include "syscall_exception.h"

#include <cstddef>

namespace psx::syscall_exception {
namespace {

constexpr std::size_t kStatus = 12;
constexpr std::size_t kCause = 13;
constexpr std::size_t kEpc = 14;
constexpr uint32_t kCauseInterruptPending = 0x0000FF00u;
constexpr uint32_t kSyscallExceptionCode = 8u;
constexpr uint32_t kCurrentModeStack = 0x3Fu;
constexpr uint32_t kReturnModeStack = 0x0Fu;
constexpr uint32_t kPreviousInterruptEnable = 1u << 2;

} // namespace

void enter(uint32_t (&cop0)[16], uint32_t syscallPc) {
  cop0[kEpc] = syscallPc;
  cop0[kStatus] = (cop0[kStatus] & ~kCurrentModeStack) | ((cop0[kStatus] << 2) & kCurrentModeStack);
  cop0[kCause] = (cop0[kCause] & kCauseInterruptPending) | (kSyscallExceptionCode << 2);
}

void setReturnInterruptEnabled(uint32_t (&cop0)[16], bool enabled) {
  if (enabled) {
    cop0[kStatus] |= kPreviousInterruptEnable;
  } else {
    cop0[kStatus] &= ~kPreviousInterruptEnable;
  }
}

void leave(uint32_t (&cop0)[16]) {
  cop0[kStatus] = (cop0[kStatus] & ~kReturnModeStack) | ((cop0[kStatus] >> 2) & kReturnModeStack);
}

} // namespace psx::syscall_exception
