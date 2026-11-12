// horizontal_margins.h — picture columns beside a 4:3 display buffer, and the one rule fitting them to a widening.
#pragma once

namespace psx::present {

// Columns of picture beside the 4:3 window, each side.
struct HorizontalMargins {
  int left = 0;
  int right = 0;

  int total() const {
    return left + right;
  }
  bool operator==(const HorizontalMargins &) const = default;
};

// A side with no world edge: more picture than any widening.
inline constexpr int kUnboundedPicture = 1 << 20;

// A widening of `margin` columns each side fitted to the picture that exists beyond the buffer: a side that runs
// short gives its columns to the other, and what neither side holds is left out (the presenter pillarboxes it).
inline HorizontalMargins fitMargins(HorizontalMargins picture, int margin) {
  const int span = 2 * (margin > 0 ? margin : 0);
  const int leftRoom = picture.left < 0 ? 0 : (picture.left > span ? span : picture.left);
  const int rightRoom = picture.right < 0 ? 0 : (picture.right > span ? span : picture.right);
  const int half = span / 2;
  int left = leftRoom < half ? leftRoom : half;
  const int right = rightRoom < span - left ? rightRoom : span - left;
  left = leftRoom < span - right ? leftRoom : span - right;
  return {left, right};
}

} // namespace psx::present
