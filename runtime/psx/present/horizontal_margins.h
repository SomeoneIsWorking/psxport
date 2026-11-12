// horizontal_margins.h — picture columns beside a 4:3 display buffer, and the one rule fitting them to a widening.
#pragma once

#include <algorithm>

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

// What a title's frame holds beside its display buffer, each side: how far its view reaches, which places the
// widening, and how much of that there is picture to draw.
struct PictureBeyondBuffer {
  HorizontalMargins view{kUnboundedPicture, kUnboundedPicture};
  HorizontalMargins picture{kUnboundedPicture, kUnboundedPicture};
};

// A record's windows beside its buffer: `view` is the whole widening as it sits on the sink (its sides sum to 2M),
// `drawn` the columns of it the record draws; the rest of the view is pillarboxed in place.
struct RecordWindows {
  HorizontalMargins view;
  HorizontalMargins drawn;

  bool operator==(const RecordWindows &) const = default;
};

// A widening of `margin` columns each side fitted to the picture that exists beyond the buffer: a side that runs
// short gives its columns to the other, and what neither side holds is left out.
inline HorizontalMargins fitMargins(HorizontalMargins picture, int margin) {
  const int span = 2 * std::max(margin, 0);
  const int leftRoom = std::clamp(picture.left, 0, span);
  const int rightRoom = std::clamp(picture.right, 0, span);
  int left = std::min(leftRoom, span / 2);
  const int right = std::min(rightRoom, span - left);
  left = std::min(leftRoom, span - right);
  return {left, right};
}

// The view takes the widening fitted to its reach, a view short on both sides centred on the rest; the record draws
// the part of it the picture covers.
inline RecordWindows fitWindows(const PictureBeyondBuffer &beyond, int margin) {
  const HorizontalMargins reached = fitMargins(beyond.view, margin);
  const int spare = 2 * std::max(margin, 0) - reached.total();
  const HorizontalMargins view{reached.left + spare / 2, reached.right + spare - spare / 2};
  const HorizontalMargins drawn{std::clamp(beyond.picture.left, 0, reached.left),
                                std::clamp(beyond.picture.right, 0, reached.right)};
  return {view, drawn};
}

} // namespace psx::present
