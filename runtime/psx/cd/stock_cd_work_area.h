// stock_cd_work_area.h — the guest bookkeeping stock libcd's command-send routine keeps.
#pragma once

#include "stock_cd_command.h"

#include <cstdint>

class Core;

namespace psx::cd {

// Where libcd keeps it; a zero address leaves that state guest-owned.
struct StockCommandWorkArea {
  uint32_t lastPositionAddress = 0; // four Setloc parameter bytes, including the trailing byte
  uint32_t lastModeAddress = 0;     // one Setmode parameter byte
  uint32_t lastCommandAddress = 0;  // one byte, every command sent; CdLastCom() reads it
};

// Writes what the replaced command-send would have recorded for `command`.
void publishStockCommandWorkArea(Core &core, const StockCommandWorkArea &workArea, StockCdCommand command);

} // namespace psx::cd
