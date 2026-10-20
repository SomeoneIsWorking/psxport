#pragma once

#include "stock_cd_command.h"

#include <cstdint>

class Core;

namespace psx::cd {

// Publish a stock libcd command's synchronous response, retaining it for the matching CdSync. False means the
// requested TOC or head position cannot be read.
bool beginStockCommand(Core &core, StockCdCommand command);
void zeroStockResult(Core &core, uint32_t result);

} // namespace psx::cd
