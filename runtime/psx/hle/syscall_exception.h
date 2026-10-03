// Native syscall exception entry/return mechanics.
//
// The shipping runtime handles kernel operations in C++, but the CPU-visible exception record is
// still guest state: Cause/EPC survive the return and Status passes through one R3000A exception
// stack level. Keep that mechanism separate from HLE selector policy so every guest executor
// and every title share one definition.
#pragma once

#include <cstdint>

namespace psx::syscall_exception {

void enter(uint32_t (&cop0)[16], uint32_t syscallPc);
void setReturnInterruptEnabled(uint32_t (&cop0)[16], bool enabled);
void leave(uint32_t (&cop0)[16]);

} // namespace psx::syscall_exception
