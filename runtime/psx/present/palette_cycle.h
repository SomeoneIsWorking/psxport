// palette_cycle.h — the CLUT rows a title's palette cycles upload, one frame of a guest table per step, so an
// in-between cross-fades such a row between the frames its two ticks showed (RecordCluts).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

class Core;

namespace psx::present {

// A 16-entry CLUT row at (x, y) its owner fills with one of `count` frames laid 32 bytes apart from `frames`.
struct PaletteCycle {
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint32_t frames = 0;
  std::uint32_t count = 0;
};

// The title's palette cycle owners.
class PaletteCycles {
public:
  virtual ~PaletteCycles() = default;
  // The rows the owners resident now upload, appended to `out`.
  virtual void declare(Core &core, std::vector<PaletteCycle> &out) const = 0;
};

// The declared rows with their frames' entries, read from guest memory at one sample.
class CycledRows {
public:
  static constexpr std::size_t kEntries = 16;

  void sample(Core &core, const PaletteCycles *cycles);
  // Whether `from` and `to` are both frames of the row at (x, y): a cycle step an in-between cross-fades.
  [[nodiscard]] bool steps(int x, int y, std::span<const std::uint16_t> from, std::span<const std::uint16_t> to) const;

private:
  struct Row {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::size_t first = 0; // into entries_
    std::uint32_t count = 0;
  };
  [[nodiscard]] bool holds(const Row &row, std::span<const std::uint16_t> entries) const;

  std::vector<PaletteCycle> declared_;
  std::vector<Row> rows_;
  std::vector<std::uint16_t> entries_;
};

} // namespace psx::present
