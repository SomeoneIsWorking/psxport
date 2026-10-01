#include "cd_stock_read_completion.h"

#include "cd_control.h"
#include "cd_ready_delivery.h"
#include "cdc_state.h"
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_cd_stream_callback_layout.h"
#include "hle.h"
#include "irq_edge.h"

#include <lucent/log.h>

namespace psx::cd {

bool stockReadOwesCompletion(const Core &core) {
  if (!cdReadyCallbackOwnedByGuestInterrupt(core)) {
    return false;
  }
  return core.game->runtime->guestCdStreamCallbackLayout()->stockReadRaisesCompletion;
}

void announceStockReadLanding(Core &core, const StockReadLanding &landing) {
  if (landing.bytes == 0u || core.game == nullptr || core.game->runtime == nullptr) {
    return;
  }
  core.game->runtime->stockCdReadLanded(core, landing);
}

bool raiseStockReadCompletion(Core &core, std::uint32_t firstLba, std::uint32_t sectors, std::uint8_t mode) {
  if (sectors == 0u || !stockReadOwesCompletion(core)) {
    return false;
  }
  if (cdc_post_data_ready_after_read(&core.game->cdc, firstLba, sectors, mode) == 0) {
    lucent::error("cd",
                  "stock CdRead of {} sector(s) completed but its completion could NOT be queued: the controller "
                  "response queue is full, so the guest's ready callback will not run for this read",
                  sectors);
    return false;
  }
  // The completion is owed to the controller's drive clock, not announced yet: `Timing` services it as
  // guest time passes and latches the edge it raises. Announcing at once made the read finish before the
  // guest could observe it in flight (see cd_stock_read_completion.h).
  const Hle &hle = core.game->hle;
  lucent::debug("cdirq",
                "stock CdRead of {} sector(s) owes its data-ready completion after the drive time (I_STAT=0x{:03X} "
                "I_MASK=0x{:03X} "
                "interrupts {}, slot holds 0x{:08X})",
                sectors,
                hle.i_stat,
                hle.i_mask,
                hle.irq_enabled ? "enabled" : "disabled",
                cd_ready_callback_pointer(core) ? core.mem_r32(cd_ready_callback_pointer(core)) : 0u);
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
