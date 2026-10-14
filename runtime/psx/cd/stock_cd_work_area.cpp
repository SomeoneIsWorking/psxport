#include "stock_cd_work_area.h"

#include "core.h"

namespace psx::cd {

void publishStockCommandWorkArea(Core &core,
                                 const StockCommandWorkArea &workArea,
                                 uint8_t command,
                                 uint32_t parameter) {
  if (workArea.lastCommandAddress) {
    core.mem_w8(workArea.lastCommandAddress, command);
  }
  if (!parameter) {
    return;
  }
  if (command == 0x02u && workArea.lastPositionAddress) {
    for (uint32_t index = 0; index < 4u; ++index) {
      core.mem_w8(workArea.lastPositionAddress + index, core.mem_r8(parameter + index));
    }
  } else if (command == 0x0Eu && workArea.lastModeAddress) {
    core.mem_w8(workArea.lastModeAddress, core.mem_r8(parameter));
  }
}

} // namespace psx::cd
