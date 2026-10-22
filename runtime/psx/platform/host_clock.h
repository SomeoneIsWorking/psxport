// host_clock.h — the host's monotonic wall clock.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace psx::platform {

using HostClock = std::chrono::steady_clock;

inline std::uint64_t hostNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now().time_since_epoch()).count());
}

// `at` in the ms hostNowMs counts.
inline double hostMs(HostClock::time_point at) {
  return std::chrono::duration<double, std::milli>(at.time_since_epoch()).count();
}

inline double hostNowMs() {
  return hostMs(HostClock::now());
}

// Sleeps until the host clock reaches `dueMs`; a deadline already passed returns at once.
inline void sleepUntilMs(double dueMs) {
  const double waitMs = dueMs - hostNowMs();
  if (waitMs > 0.0) {
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(waitMs));
  }
}

inline double msBetween(HostClock::time_point from, HostClock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace psx::platform
