#include "cd_drive_timing.h"

int cd_drive_sectors_per_second(uint8_t mode) {
  return (mode & 0x80u) != 0 ? 150 : 75;
}

uint64_t cd_drive_sector_period_cpu_ticks(uint8_t mode) {
  return kNominalPsxCpuHz / static_cast<uint64_t>(cd_drive_sectors_per_second(mode));
}
