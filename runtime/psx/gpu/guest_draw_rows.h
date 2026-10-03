#pragma once

#include <array>
#include <optional>

// WHICH VRAM ROWS THE GUEST DRAWS INTO EACH OF ITS FRAMEBUFFERS.
//
// A double-buffered guest alternates draw areas (GP0 E3/E4) between two framebuffers, and a
// letterboxed one draws fewer rows than it displays: Spyro 2 displays 240 rows from (0,0) and (0,228)
// but draws rows 12..227 and 240..455. The presenter needs the draw rows of the buffer it is SHOWING,
// which is not the draw area current at present time: that one is the buffer being drawn next. This
// keeps the few most recent distinct row spans, newest first, so the displayed buffer's own span is
// the newest one that intersects its display window.
namespace psx::gpu {

// VRAM rows [top, bottom).
struct RowSpan {
  int top = 0;
  int bottom = 0;

  bool operator==(const RowSpan &) const = default;

  bool intersects(RowSpan other) const {
    return top < other.bottom && other.top < bottom;
  }
};

class RecentDrawRows {
public:
  // Enough for triple buffering plus one transitional area; the oldest span is evicted first.
  static constexpr int kCapacity = 4;

  // Record a draw area's rows as the newest. A span already held moves to the front instead of
  // occupying a second slot, so one buffer re-issuing its own area cannot evict the other's.
  void note(RowSpan rows) {
    int at = 0;
    while (at < count_ && !(spans_[at] == rows)) {
      ++at;
    }
    if (at == count_) {
      at = count_ < kCapacity ? count_++ : kCapacity - 1;
    }
    for (int i = at; i > 0; --i) {
      spans_[i] = spans_[i - 1];
    }
    spans_[0] = rows;
  }

  // The newest recorded span that shares a row with `window`, or none when the guest has drawn into
  // no row of it.
  std::optional<RowSpan> newestIntersecting(RowSpan window) const {
    for (int i = 0; i < count_; ++i) {
      if (spans_[i].intersects(window)) {
        return spans_[i];
      }
    }
    return std::nullopt;
  }

private:
  std::array<RowSpan, kCapacity> spans_{};
  int count_ = 0;
};

} // namespace psx::gpu
