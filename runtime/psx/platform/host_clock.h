// host_clock.h — the host's monotonic wall clock.
#pragma once

#include <chrono>
#include <cstdint>

namespace psx::platform {

using HostClock = std::chrono::steady_clock;

inline std::uint64_t hostNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now().time_since_epoch()).count());
}

inline double hostNowMs() {
  return std::chrono::duration<double, std::milli>(HostClock::now().time_since_epoch()).count();
}

inline double msBetween(HostClock::time_point from, HostClock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace psx::platform
