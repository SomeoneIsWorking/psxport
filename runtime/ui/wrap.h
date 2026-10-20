// psx::ui::wrap — stepping a selection along a row that wraps at both ends.
#pragma once

namespace psx::ui {

// `value` moved by `dir` within [0, span); span < 1 leaves it unchanged.
constexpr int wrap(int value, int dir, int span) {
  if (span < 1) {
    return value;
  }
  return ((value + dir) % span + span) % span;
}

} // namespace psx::ui
