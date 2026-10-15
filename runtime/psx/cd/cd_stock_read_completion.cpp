#include "cd_stock_read_completion.h"

#include "cdc_state.h"
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "hle.h"
#include "irq_edge.h"

#include <lucent/log.h>

namespace psx::cd {

void announceStockReadLanding(Core &core, const StockReadLanding &landing) {
  if (landing.bytes == 0u) {
    return;
  }
  core.runtime.stockCdReadLanded(core, landing);
}

bool raiseStockReadCompletion(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode) {
  if (sectors == 0u) {
    return false;
  }
  if (core.game->cdc.postDataReadyAfterRead(firstLba, sectors, mode) == 0) {
    lucent::error("cd",
                  "stock CdRead of {} sector(s) completed but its completion could NOT be queued: the controller "
                  "response queue is full, so the guest's ready callback will not run for this read",
                  sectors);
    return false;
  }
  // Owed to the drive clock, which psx::frame::Timing services, so the guest sees the read in flight.
  const Hle &hle = core.game->hle;
  lucent::debug("cdirq",
                "stock CdRead of {} sector(s) owes its data-ready completion after the drive time (I_STAT=0x{:03X} "
                "I_MASK=0x{:03X}, interrupts {})",
                sectors,
                hle.i_stat,
                hle.i_mask,
                hle.irq_enabled ? "enabled" : "disabled");
  return true;
}

bool armCdInterrupt(Core &core) {
  // Through the device, not the host's mask field, so the framework owns the value and re-arms its own
  // delivery gate; the current value is read back so the guest's other enables survive.
  const std::uint32_t before = core.mem_r32(kIrqMaskRegister);
  const std::uint32_t after = before | (1u << IRQ_BIT_CD);
  if (after == before) {
    return false;
  }
  core.mem_w32(kIrqMaskRegister, after);
  lucent::debug("cdirq", "CD interrupt armed: I_MASK 0x{:03X} -> 0x{:03X}", before, after);
  return true;
}

} // namespace psx::cd
