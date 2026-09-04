// bios_interrupt.cpp — the Sony BIOS HookEntryInt jmp_buf contract.
#include "bios_interrupt.h"
#include "core.h"
#include <cstdlib>
#include <lucent/log.h>

namespace {
enum { V0 = 2, S0 = 16, GP = 28, SP = 29, FP = 30 };

constexpr uint32_t kResumeOffset = 0x00u;
constexpr uint32_t kSpOffset = 0x04u;
constexpr uint32_t kFpOffset = 0x08u;
constexpr uint32_t kS0Offset = 0x0Cu;
constexpr uint32_t kGpOffset = 0x2Cu;

struct ReturnFromException {};
} // namespace

void bios_interrupt_trace_custom_exit(Core *c, uint32_t buffer) {
  if (!buffer || !lucent::channel_on("bios")) {
    return;
  }
  lucent::debug("bios",
                "B0:0x19 custom-exit buf=0x{:08X}: ra=0x{:08X} sp=0x{:08X} fp=0x{:08X} gp=0x{:08X}",
                buffer,
                c->mem_r32(buffer + kResumeOffset),
                c->mem_r32(buffer + kSpOffset),
                c->mem_r32(buffer + kFpOffset),
                c->mem_r32(buffer + kGpOffset));
  for (uint32_t i = 0; i < 8; i++) {
    lucent::debug("bios", "  s{} = 0x{:08X}", i, c->mem_r32(buffer + kS0Offset + 4u * i));
  }
}

uint32_t bios_interrupt_enter_custom_exit(Core *c, uint32_t buffer) {
  if (!buffer) {
    return 0;
  }
  const uint32_t resume = c->mem_r32(buffer + kResumeOffset);
  if (!resume) {
    return 0;
  }

  c->r[SP] = c->mem_r32(buffer + kSpOffset);
  c->r[FP] = c->mem_r32(buffer + kFpOffset);
  for (uint32_t i = 0; i < 8; i++) {
    c->r[S0 + i] = c->mem_r32(buffer + kS0Offset + 4u * i);
  }
  c->r[GP] = c->mem_r32(buffer + kGpOffset);
  c->r[V0] = 1;
  return resume;
}

BiosInterruptDispatchResult
bios_interrupt_dispatch_custom_exit(Core *c, uint32_t buffer, BiosInterruptGuestDispatch dispatch) {
  const uint32_t resume = bios_interrupt_enter_custom_exit(c, buffer);
  if (!resume || !dispatch) {
    return BiosInterruptDispatchResult::Refused;
  }
  try {
    dispatch(c, resume);
  } catch (const ReturnFromException &) {
    return BiosInterruptDispatchResult::ReturnedFromException;
  }
  return BiosInterruptDispatchResult::FellThrough;
}

void bios_interrupt_return_from_exception(bool custom_exit_active) {
  if (!custom_exit_active) {
    lucent::error("irq", "B0:0x17 ReturnFromException called without an active custom exit");
    abort();
  }
  throw ReturnFromException{};
}
