// write_rollback.h — the guest bytes a run overwrote and what they held, so the run can be undone.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace psx {

// While armed, every guest store notes the bytes it is about to overwrite; `rollBack` writes them back,
// newest first, and disarms. The log keeps its capacity from run to run.
class WriteRollback {
public:
  [[nodiscard]] bool armed() const {
    return armed_;
  }
  void arm() {
    entries_.clear();
    armed_ = true;
  }
  void note(std::uint8_t *at, std::uint32_t width) {
    Entry entry{.at = at, .width = width};
    std::memcpy(entry.held.data(), at, width);
    entries_.push_back(entry);
  }
  void rollBack() {
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
      std::memcpy(entry->at, entry->held.data(), entry->width);
    }
    entries_.clear();
    armed_ = false;
  }

private:
  struct Entry {
    std::uint8_t *at = nullptr;
    std::uint32_t width = 0;
    std::array<std::uint8_t, 4> held{};
  };
  std::vector<Entry> entries_;
  bool armed_ = false;
};

} // namespace psx
