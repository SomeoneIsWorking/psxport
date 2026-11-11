#include "palette_cycle.h"

#include "core.h"

#include <algorithm>

namespace psx::present {

void CycledRows::sample(Core &core, const PaletteCycles *cycles) {
  declared_.clear();
  rows_.clear();
  entries_.clear();
  if (cycles == nullptr) {
    return;
  }
  cycles->declare(core, declared_);
  for (const PaletteCycle &cycle : declared_) {
    rows_.push_back({.x = cycle.x, .y = cycle.y, .first = entries_.size(), .count = cycle.count});
    for (std::uint32_t i = 0; i < cycle.count * kEntries; i++) {
      entries_.push_back(core.mem_r16(cycle.frames + i * 2u));
    }
  }
}

bool CycledRows::holds(const Row &row, std::span<const std::uint16_t> entries) const {
  for (std::uint32_t frame = 0; frame < row.count; frame++) {
    const auto at = entries_.begin() + static_cast<std::ptrdiff_t>(row.first + frame * kEntries);
    if (std::equal(entries.begin(), entries.end(), at, at + kEntries)) {
      return true;
    }
  }
  return false;
}

bool CycledRows::steps(int x, int y, std::span<const std::uint16_t> from, std::span<const std::uint16_t> to) const {
  if (from.size() != kEntries || to.size() != kEntries) {
    return false;
  }
  return std::any_of(rows_.begin(), rows_.end(), [&](const Row &row) {
    return row.x == x && row.y == y && holds(row, from) && holds(row, to);
  });
}

} // namespace psx::present
