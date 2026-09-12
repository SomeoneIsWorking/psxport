// BIOS PadCardIrq enable/disable entry points reached through B0[5B]'s work area.
#include "hle.h"

#include "game.h"

namespace {

constexpr uint32_t kPadEnableOffset = 0x884u;
constexpr uint32_t kPadDisableOffset = 0x894u;
constexpr uint32_t kB0WorkBaseSlot = Hle::kB0Table + 0x5Bu * sizeof(uint32_t);
// SCPH-1001 v2.2 (SHA-1 10155d8d6e6e832d6ea66db9bc098321fb5e8ebf): ROM offsets 0x14754
// and 0x14764 align with work-area +884/+894. Both store to kernel word 0x74B8 (1 then 0) and
// return without modifying V0. This is guest RAM, not private HLE state: libpad and guest patches
// can read or change the flag directly.
constexpr uint32_t kPadEnableFlag = 0x000074B8u;

} // namespace

bool Hle::dispatchPadBios(uint32_t function) {
  switch (function) {
  case 0x12u: // InitPAD2 prepares and enables the BIOS pad handler.
    bios_pad_initialized = true;
    bios_pad_irq_started = false;
    applyPadWorkAreaAction(PadWorkAreaAction::Enable);
    break;
  case 0x13u: // StartPAD2 enqueues the pad/card IRQ handler.
    bios_pad_irq_started = true;
    break;
  case 0x14u: // StopPAD2 dequeues it without losing the registered buffers.
    bios_pad_irq_started = false;
    break;
  default:
    return false;
  }
  game->core.r[2] = 0;
  return true;
}

std::optional<Hle::PadWorkAreaAction> Hle::padWorkAreaAction(uint32_t guestAddress) const {
  // The callback belongs to the HLE BIOS only after GetB0Table published the work area. A title
  // that changes the table's base must not silently dispatch the old synthetic service.
  if (!work_ok || game->core.mem_r32(kB0WorkBaseSlot) != kWorkBase) {
    return std::nullopt;
  }
  const uint32_t physical = guestAddress & 0x1FFFFFFFu;
  switch (physical) {
  case (kWorkBase + kPadEnableOffset) & 0x1FFFFFFFu:
    return PadWorkAreaAction::Enable;
  case (kWorkBase + kPadDisableOffset) & 0x1FFFFFFFu:
    return PadWorkAreaAction::Disable;
  default:
    return std::nullopt;
  }
}

void Hle::applyPadWorkAreaAction(PadWorkAreaAction action) {
  game->core.mem_w32(kPadEnableFlag, action == PadWorkAreaAction::Enable ? 1u : 0u);
}

bool Hle::biosPadShouldService() const {
  return !bios_pad_initialized || (bios_pad_irq_started && game->core.mem_r32(kPadEnableFlag) != 0u);
}
