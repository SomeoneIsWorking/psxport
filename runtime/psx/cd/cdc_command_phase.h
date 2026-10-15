// Oracle-derived CD command receive/argument/execute timing.
#pragma once

#include <cstdint>

namespace psx::cd {

inline constexpr uint64_t kCdcCommandWritePhaseCpuTicks = 10'500u + 1'815u;
inline constexpr uint64_t kCdcCommandArgumentPhaseCpuTicks = 1'815u;
inline constexpr uint64_t kCdcCommandExecutionPhaseCpuTicks = 8'500u;

constexpr uint64_t commandAckDelayCpuTicks(uint8_t argument_count) {
  return kCdcCommandWritePhaseCpuTicks + static_cast<uint64_t>(argument_count) * kCdcCommandArgumentPhaseCpuTicks +
         kCdcCommandExecutionPhaseCpuTicks;
}

} // namespace psx::cd
