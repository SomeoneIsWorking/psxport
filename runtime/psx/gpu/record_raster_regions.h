// record_raster_regions.h — native VRAM rectangles for the record planner: wrapping and written bits.
#pragma once

#include "record_geometry.h"
#include "vram_bitmap.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace psx::gpu {

inline int positiveMod(int value, int modulus) {
  const int wrapped = value % modulus;
  return wrapped < 0 ? wrapped + modulus : wrapped;
}

// At most N values, held inline.
template <typename T, std::size_t N> class FixedList {
public:
  void push_back(const T &value) {
    items_[count_++] = value;
  }
  const T *begin() const {
    return items_.data();
  }
  const T *end() const {
    return items_.data() + count_;
  }

private:
  std::array<T, N> items_{};
  std::size_t count_ = 0;
};

// Bits of a plane (VRAM or a canvas, at most VRAM-sized) written by the ops of the open batch.
class WrittenMap {
public:
  bool any(const RecordRect &rect) const {
    if (empty_ || rect.x1 <= rect.x0 || rect.y1 <= rect.y0 || rect.x1 <= bounds_.x0 || rect.x0 >= bounds_.x1 ||
        rect.y1 <= bounds_.y0 || rect.y0 >= bounds_.y1) {
      return false;
    }
    const int y0 = std::max(rect.y0, bounds_.y0);
    const int y1 = std::min(rect.y1, bounds_.y1);
    const int x0 = std::max(rect.x0, bounds_.x0);
    const int x1 = std::min(rect.x1, bounds_.x1);
    for (int y = y0; y < y1; y++) {
      if (bits_.any(y, {x0, x1})) {
        return true;
      }
    }
    return false;
  }

  void mark(const RecordRect &rect) {
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
      return;
    }
    for (int y = rect.y0; y < rect.y1; y++) {
      bits_.mark(y, {rect.x0, rect.x1});
    }
    if (empty_) {
      bounds_ = rect;
      empty_ = false;
    } else {
      bounds_ = RecordRect{std::min(bounds_.x0, rect.x0),
                           std::min(bounds_.y0, rect.y0),
                           std::max(bounds_.x1, rect.x1),
                           std::max(bounds_.y1, rect.y1)};
    }
  }

  bool empty() const {
    return empty_;
  }
  RecordRect bounds() const {
    return bounds_;
  }

  void clear() {
    if (empty_) {
      return;
    }
    for (int y = bounds_.y0; y < bounds_.y1; y++) {
      bits_.clearRow(y);
    }
    empty_ = true;
  }

private:
  VramBitmap bits_;
  RecordRect bounds_;
  bool empty_ = true;
};

// `length` units from `start` on a ring of `modulus` units.
struct RingRun {
  int start = 0;
  int length = 0;

  // At most two runs [first, second) inside [0, modulus).
  int split(int modulus, std::pair<int, int> out[2]) const {
    if (length <= 0) {
      return 0;
    }
    const int from = positiveMod(start, modulus);
    const int units = std::min(length, modulus);
    const int first = std::min(units, modulus - from);
    out[0] = {from, from + first};
    if (first == units) {
      return 1;
    }
    out[1] = {0, units - first};
    return 2;
  }

  bool overlaps(RingRun other, int modulus) const {
    std::pair<int, int> ra[2];
    std::pair<int, int> rb[2];
    const int na = split(modulus, ra);
    const int nb = other.split(modulus, rb);
    for (int i = 0; i < na; i++) {
      for (int j = 0; j < nb; j++) {
        if (ra[i].first < rb[j].second && rb[j].first < ra[i].second) {
          return true;
        }
      }
    }
    return false;
  }
};

using WrappedRects = FixedList<RecordRect, 4>;

// The native rectangles a wrapped VRAM rectangle covers.
inline WrappedRects wrappedRects(VramRect area) {
  std::pair<int, int> columns[2];
  std::pair<int, int> rows[2];
  const int nc = RingRun{area.x, area.width}.split(kVramWidth, columns);
  const int nr = RingRun{area.y, area.height}.split(kVramHeight, rows);
  WrappedRects rects;
  for (int r = 0; r < nr; r++) {
    for (int c = 0; c < nc; c++) {
      rects.push_back(RecordRect{columns[c].first, rows[r].first, columns[c].second, rows[r].second});
    }
  }
  return rects;
}

} // namespace psx::gpu
