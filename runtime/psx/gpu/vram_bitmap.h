// vram_bitmap.h — one bit per VRAM pixel, marked and tested a row's column run at a time.
#pragma once

#include "record_raster_setup.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace psx::gpu {

// Columns [x0, x1) of one row, 0 <= x0 <= x1 <= kRecordVramWidth.
struct ColumnRun {
  int x0 = 0;
  int x1 = 0;
};

class VramBitmap {
public:
  VramBitmap() : words_(static_cast<std::size_t>(kRecordVramHeight) * kWordsPerRow, 0) {}

  void mark(int row, ColumnRun run) {
    forEachWord(words_, row, run, [](std::uint64_t &word, std::uint64_t mask) {
      word |= mask;
      return false;
    });
  }

  bool any(int row, ColumnRun run) const {
    return forEachWord(words_, row, run, [](const std::uint64_t &word, std::uint64_t mask) {
      return (word & mask) != 0;
    });
  }

  void fill(bool set) {
    std::fill(words_.begin(), words_.end(), set ? ~std::uint64_t{0} : std::uint64_t{0});
  }

  void clearRow(int row) {
    std::fill_n(words_.begin() + static_cast<std::ptrdiff_t>(row) * kWordsPerRow, kWordsPerRow, std::uint64_t{0});
  }

private:
  static constexpr int kWordsPerRow = kRecordVramWidth / 64;

  // Bits [x0, x1) of one word, 0 <= x0 < x1 <= 64.
  static std::uint64_t wordMask(ColumnRun bits) {
    const int count = bits.x1 - bits.x0;
    const std::uint64_t ones = count == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1u;
    return ones << bits.x0;
  }

  // Calls `visit(word, mask)` for each word the run touches; stops when it returns true.
  template <typename Words, typename Visit> static bool forEachWord(Words &words, int row, ColumnRun run, Visit visit) {
    const std::size_t base = static_cast<std::size_t>(row) * kWordsPerRow;
    for (int x = run.x0; x < run.x1;) {
      const int bit = x % 64;
      const int end = std::min(64, bit + run.x1 - x);
      if (visit(words[base + static_cast<std::size_t>(x / 64)], wordMask({bit, end}))) {
        return true;
      }
      x += end - bit;
    }
    return false;
  }

  std::vector<std::uint64_t> words_;
};

} // namespace psx::gpu
