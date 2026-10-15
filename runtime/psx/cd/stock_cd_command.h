// stock_cd_command.h — one stock libcd controller command as the guest passed it.
#pragma once

#include <cstdint>

namespace psx::cd {

// The command byte, its guest parameter block and its guest result buffer; a zero address is absent.
struct StockCdCommand {
  uint8_t command = 0;
  uint32_t parameter = 0;
  uint32_t result = 0;
};

} // namespace psx::cd
