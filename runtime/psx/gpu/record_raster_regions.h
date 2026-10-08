// record_raster_regions.h — native VRAM rectangles for the record planner: wrapping and written bits.
#pragma once

#include "record_raster_setup.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace psx::gpu {

inline int positiveMod(int value, int modulus) {
  const int wrapped = value % modulus;
  return wrapped < 0 ? wrapped + modulus : wrapped;
}

// Bits of a plane (VRAM or a canvas, at most VRAM-sized) written by the ops of the open batch.
class WrittenMap {
public:
  WrittenMap() : bits_(static_cast<std::size_t>(kRecordVramHeight) * kWordsPerRow, 0) {}

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
      if (rowAny(y, x0, x1)) {
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
      for (int x = rect.x0; x < rect.x1;) {
        const int word = x >> 6;
        const int end = std::min(rect.x1, (word + 1) << 6);
        bits_[static_cast<std::size_t>(y) * kWordsPerRow + static_cast<std::size_t>(word)] |=
            rangeMask(x & 63, end - (word << 6));
        x = end;
      }
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
      std::fill_n(bits_.begin() + static_cast<std::ptrdiff_t>(y) * kWordsPerRow, kWordsPerRow, 0);
    }
    empty_ = true;
  }

private:
  static constexpr int kWordsPerRow = kRecordVramWidth / 64;

  static std::uint64_t rangeMask(int from, int to) {
    const std::uint64_t high = to >= 64 ? ~0ull : ((1ull << to) - 1ull);
    return high & ~((1ull << from) - 1ull);
  }

  bool rowAny(int y, int x0, int x1) const {
    for (int x = x0; x < x1;) {
      const int word = x >> 6;
      const int end = std::min(x1, (word + 1) << 6);
      if ((bits_[static_cast<std::size_t>(y) * kWordsPerRow + static_cast<std::size_t>(word)] &
           rangeMask(x & 63, end - (word << 6))) != 0) {
        return true;
      }
      x = end;
    }
    return false;
  }

  std::vector<std::uint64_t> bits_;
  RecordRect bounds_;
  bool empty_ = true;
};

// Splits a span of `length` starting at `start` on a ring of `modulus` into at most two runs.
inline int splitRing(int start, int length, int modulus, std::pair<int, int> out[2]) {
  if (length <= 0) {
    return 0;
  }
  start = positiveMod(start, modulus);
  length = std::min(length, modulus);
  const int first = std::min(length, modulus - start);
  out[0] = {start, start + first};
  if (first == length) {
    return 1;
  }
  out[1] = {0, length - first};
  return 2;
}

// The native rectangles a wrapped VRAM rectangle covers.
inline std::vector<RecordRect> wrappedRects(int x, int y, int width, int height) {
  std::pair<int, int> columns[2];
  std::pair<int, int> rows[2];
  const int nc = splitRing(x, width, kRecordVramWidth, columns);
  const int nr = splitRing(y, height, kRecordVramHeight, rows);
  std::vector<RecordRect> rects;
  for (int r = 0; r < nr; r++) {
    for (int c = 0; c < nc; c++) {
      rects.push_back(RecordRect{columns[c].first, rows[r].first, columns[c].second, rows[r].second});
    }
  }
  return rects;
}

inline bool ringsOverlap(int a, int lengthA, int b, int lengthB, int modulus) {
  std::pair<int, int> ra[2];
  std::pair<int, int> rb[2];
  const int na = splitRing(a, lengthA, modulus, ra);
  const int nb = splitRing(b, lengthB, modulus, rb);
  for (int i = 0; i < na; i++) {
    for (int j = 0; j < nb; j++) {
      if (ra[i].first < rb[j].second && rb[j].first < ra[i].second) {
        return true;
      }
    }
  }
  return false;
}

} // namespace psx::gpu
