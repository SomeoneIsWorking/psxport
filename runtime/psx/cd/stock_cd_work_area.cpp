#include "stock_cd_work_area.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "legacy_game_config.h"
#include "platform_hle.h"

namespace psx::cd {
namespace {

StockCommandWorkArea resolveWorkArea(const Core &core) {
  if (core.cfg) {
    uint32_t base = core.cfg->cdLastPosBuf;
    return base ? StockCommandWorkArea{base, base + 4u} : StockCommandWorkArea{};
  }
  auto *runtime = core.game ? core.game->runtime : nullptr;
  auto *plan = runtime ? runtime->platformHlePlan() : nullptr;
  return plan ? plan->stockCdWorkArea : StockCommandWorkArea{};
}

} // namespace

void publishStockCommandWorkArea(Core &core, uint8_t command, uint32_t parameter) {
  if (!parameter) {
    return;
  }

  StockCommandWorkArea workArea = resolveWorkArea(core);
  if (command == 0x02u && workArea.lastPositionAddress) {
    for (uint32_t index = 0; index < 4u; ++index) {
      core.mem_w8(workArea.lastPositionAddress + index, core.mem_r8(parameter + index));
    }
  } else if (command == 0x0Eu && workArea.lastModeAddress) {
    core.mem_w8(workArea.lastModeAddress, core.mem_r8(parameter));
  }
}

} // namespace psx::cd
