#include "cd_position.h"

namespace psx::cd {

bool commandCarriesPosition(std::uint8_t command) {
  switch (command) {
  case 0x03: // SetlocL
  case 0x06: // ReadN
  case 0x15: // SeekL
  case 0x16: // SeekP
  case 0x1B: // ReadS
    return true;
  default:
    return false;
  }
}

} // namespace psx::cd
