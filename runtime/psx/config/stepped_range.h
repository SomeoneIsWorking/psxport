// stepped_range.h - the one rule for an integer setting that moves in fixed steps inside [min, max].
//
// A title declares the range once; the menu row steps through it and the title reads the stored value
// back through the same clamp, so a hand-edited settings file cannot hold an off-grid value.
#pragma once

namespace psx::config {

struct SteppedRange {
  long min = 0;
  long max = 0;
  long step = 1;

  // The nearest value on the step grid from `min`, never past `max`.
  constexpr long clamp(long value) const {
    if (step <= 0 || max <= min) {
      return min;
    }
    if (value <= min) {
      return min;
    }
    if (value >= max) {
      return min + (max - min) / step * step;
    }
    const long onGrid = min + (value - min + step / 2) / step * step;
    return onGrid > max ? min + (max - min) / step * step : onGrid;
  }

  // `value` moved `direction` steps (positive or negative), held at the ends.
  constexpr long stepped(long value, int direction) const {
    return clamp(clamp(value) + step * direction);
  }
};

} // namespace psx::config
