// bios_interrupt.h — Sony BIOS custom exception-exit context.
//
// HookEntryInt (B0:0x19) receives a guest jmp_buf, not a callback address. After the ordinary
// SysEnqIntRP chain, the BIOS restores this context and returns from setjmp with a non-zero value.
// The saved RA is consequently a continuation inside the game's interrupt bootstrap and must be a
// dispatchable executor entry.
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

// Log the measured fields of a newly installed context when the BIOS diagnostic channel is enabled.
void traceInterruptCustomExit(Core *c, uint32_t buffer);

// Load the stack, frame, saved-register and global-pointer context held in `buffer`: the context a
// hardware interrupt handler runs in, whatever the interrupted code left in those registers. A zero
// buffer is refused without changing Core.
bool enterInterruptHandlerContext(Core *c, uint32_t buffer);

// Restore the callee-saved register subset held in `buffer`, set V0 non-zero, and return the saved
// continuation address. A zero buffer or zero continuation is refused without changing Core.
uint32_t enterInterruptCustomExit(Core *c, uint32_t buffer);

// Dispatch the saved continuation under a scoped unwind boundary. ReturnFromException throws only
// the private marker caught here, so the current nested guest call is discarded without masking
// unrelated C++ failures. A normal return is reported separately because it violates the BIOS
// contract and means guest code fell through beyond its exception-only path.
BiosInterruptDispatchResult dispatchInterruptCustomExit(Core *c, uint32_t buffer, BiosInterruptGuestDispatch dispatch);

// B0:0x17. This is deliberately non-returning: resuming after the BIOS call could execute one-time
// interrupt initialization as though the exception bootstrap were ordinary control flow.
[[noreturn]] void returnFromException(bool custom_exit_active);

} // namespace psx::hle
