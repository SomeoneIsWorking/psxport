#include "stage_sides.h"

#include "wrap.h"

#include <algorithm>

namespace psx::host {

StageSides::StageSides(int titles) : titles_(std::max(titles, 0)) {}

int StageSides::sides() const {
  return std::min(titles_, kSides);
}

int StageSides::title(int side) const {
  if (side < 0 || side >= sides()) {
    return -1;
  }
  return side_[static_cast<std::size_t>(side)];
}

StageSides::Move StageSides::step(int dir) {
  if (dir == 0 || sides() < kSides) {
    return {};
  }
  const int toward = dir > 0 ? 1 : 0;
  if (selected_ != toward) {
    selected_ = toward;
    return {.changed = true};
  }
  if (titles_ <= kSides) {
    return {};
  }
  // The selected title steps to the other side and the next one in the ring comes in on this one.
  const int current = side_[static_cast<std::size_t>(toward)];
  side_[static_cast<std::size_t>(1 - toward)] = current;
  side_[static_cast<std::size_t>(toward)] = psx::ui::wrap(current, dir > 0 ? 1 : -1, titles_);
  return {.changed = true, .refilled = true};
}

StageSides::Move StageSides::show(int title) {
  if (title < 0 || title >= titles_) {
    return {};
  }
  for (int side = 0; side < sides(); ++side) {
    if (side_[static_cast<std::size_t>(side)] == title) {
      const bool changed = selected_ != side;
      selected_ = side;
      return {.changed = changed};
    }
  }
  const int other = 1 - selected_;
  side_[static_cast<std::size_t>(other)] = title;
  selected_ = other;
  return {.changed = true};
}

} // namespace psx::host
