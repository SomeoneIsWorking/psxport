#pragma once

#include <cstdint>

struct Core;

namespace psx::cd {

// Guest state written by stock Sony libcd's command-send routine. A zero address leaves that
// particular state guest-owned; direct titles provide the measured addresses in PlatformHlePlan.
struct StockCommandWorkArea {
  uint32_t lastPositionAddress = 0; // Four Setloc parameter bytes, including the trailing byte.
  uint32_t lastModeAddress = 0;     // One Setmode parameter byte.
};

// Preserve the replaced command routine's guest bookkeeping through the shared stock-CD path.
// The legacy adapter's contiguous base and direct runtimes' explicit addresses use one policy.
void publishStockCommandWorkArea(Core &core, uint8_t command, uint32_t parameter);

} // namespace psx::cd
