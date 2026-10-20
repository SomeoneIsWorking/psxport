#include "spin_detector.h"

#include <algorithm>

namespace psx::platform {

bool sampleSpinDetector(SpinDetectorState &st, SpinSample sample, SpinLimits limits) {
  const uint32_t pc = sample.pc;
  const bool host_starved = sample.hostStarved;
  const uint64_t window_ticks = limits.windowTicks;
  const int max_run = limits.maxRun;
  const uint32_t ticks = sample.ticks;
  if (window_ticks == 0 || max_run <= 0) {
    return false;
  }
  st.window_ticks += ticks;
  if (st.window_ticks < window_ticks) {
    return false;
  }
  st.window_ticks = 0;

  constexpr uint32_t kRegionMask = ~0x7FFFFu; // one ±512KB region
  const bool same_region = st.anchor != 0 && (pc & kRegionMask) == (st.anchor & kRegionMask);
  if (!host_starved) {
    st.anchor = pc;
    st.run = 0;
    return false;
  }
  if (!same_region) {
    // Moved while starved: a fresh candidate run anchored here.
    st.anchor = pc;
    st.run = 1;
    return st.run >= max_run;
  }
  st.run = std::min(st.run + 1, max_run + 1); // saturate; never overflow
  return st.run >= max_run;
}

} // namespace psx::platform
