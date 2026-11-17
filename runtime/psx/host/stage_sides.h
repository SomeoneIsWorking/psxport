// Which two of the selector's titles stand on the stage's two sides, and which side is selected.
#pragma once

#include <array>

namespace psx::host {

// The stage has two sides at most. With more titles they form a ring: the stage shows the selected title
// and one neighbour, and moving on past the selected side brings the next title in.
class StageSides {
public:
  static constexpr int kSides = 2;

  // Titles 0 and 1 (or 0 alone), the left one selected.
  explicit StageSides(int titles);

  // 0, 1 or 2.
  int sides() const;
  // The title standing on `side`, or -1 outside the sides.
  int title(int side) const;
  int selectedSide() const {
    return selected_;
  }
  int selectedTitle() const {
    return title(selected_);
  }

  // What a move did to the stage. `refilled`: both sides took new titles, so the edge starts again from the
  // side the move came from rather than easing from where it stood.
  struct Move {
    bool changed = false;
    bool refilled = false;
  };
  // One step left (-1) or right (+1). Two titles stop at the window's edges; a ring of three or more turns on.
  Move step(int dir);
  // Select `title`: its side when it stands on one, else it replaces the unselected side.
  Move show(int title);

private:
  int titles_;
  std::array<int, kSides> side_{0, 1};
  int selected_ = 0;
};

} // namespace psx::host
