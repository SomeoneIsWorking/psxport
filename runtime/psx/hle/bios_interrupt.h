// The Sony BIOS jmp_buf { +0 ra, +4 sp, +8 fp, +0xC s0..s7, +0x2C gp }: setjmp and the HookEntryInt exit.
#pragma once
#include <cstdint>

class Core;

using BiosInterruptGuestDispatch = void (*)(Core *, uint32_t);

enum class BiosInterruptDispatchResult : std::uint8_t {
  Refused,
  ReturnedFromException,
  FellThrough,
};

namespace psx::hle {

// A0:0x13 setjmp: the callee-saved set in the BIOS layout.
void saveJmpBuf(Core *c, uint32_t buffer);

// Logs a newly installed HookEntryInt context on the `bios` channel.
void traceInterruptCustomExit(Core *c, uint32_t buffer);

// Loads sp, fp, s0..s7 and gp from `buffer`; a zero buffer is refused without changing Core.
bool enterInterruptHandlerContext(Core *c, uint32_t buffer);

// Restores `buffer`, sets v0 = 1 and dispatches its ra; only B0:0x17's marker unwinds out of it.
BiosInterruptDispatchResult dispatchInterruptCustomExit(Core *c, uint32_t buffer, BiosInterruptGuestDispatch dispatch);

// B0:0x17; resuming after the call would run interrupt setup as ordinary control flow.
[[noreturn]] void returnFromException(bool custom_exit_active);

} // namespace psx::hle
