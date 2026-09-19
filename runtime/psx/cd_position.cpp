#include "cd_position.h"

namespace psx::cd {
namespace {

int fromBcd(std::uint8_t v) {
  return (v >> 4) * 10 + (v & 0x0F);
}

} // namespace

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

int msfToLba(std::uint8_t mm, std::uint8_t ss, std::uint8_t ff) {
  const int sector = (fromBcd(mm) * 60 + fromBcd(ss)) * 75 + fromBcd(ff);
  return sector >= kLeadInSectors ? sector - kLeadInSectors : -1;
}

} // namespace psx::cd
