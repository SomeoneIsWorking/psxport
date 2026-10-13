// How many pre-roll steps one picker frame may take.
#pragma once

#include <chrono>

namespace psx::host {

// A pre-roll fills what is left of the picker's field, so a booting panel never stretches a frame.
class PreRollBudget {
public:
  using Clock = std::chrono::steady_clock;

  explicit PreRollBudget(Clock::time_point until) : until_(until) {}

  // The first step always runs, so a step slower than a field still makes progress.
  bool mayStep(int taken, Clock::time_point now, Clock::duration lastStep) const {
    return taken == 0 || now + lastStep <= until_;
  }

private:
  Clock::time_point until_;
};

} // namespace psx::host
