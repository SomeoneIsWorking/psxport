// spin_detector.h — declares a guest spin when guest time keeps passing with the host owed turns it
// never takes (PW_HOST held) and execution never leaves one ±512KB region.
#pragma once
#include <cstdint>

namespace psx::platform {

struct SpinDetectorState {
  uint64_t window_ticks = 0; // ticks accumulated toward the next decision
  uint32_t anchor = 0;       // pc of the first sample of the current run (0 = none yet)
  int run = 0;               // consecutive starved in-region decisions
};

// One executed chunk.
struct SpinSample {
  uint32_t pc = 0;
  bool hostStarved = false;
  uint32_t ticks = 0;
};

// A zero window or run disables detection.
struct SpinLimits {
  uint64_t windowTicks = 0;
  int maxRun = 0;
};

// True exactly when a spin is declared; the caller reports and aborts.
bool sampleSpinDetector(SpinDetectorState &st, SpinSample sample, SpinLimits limits);

} // namespace psx::platform
