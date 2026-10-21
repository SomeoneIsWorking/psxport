// write_rollback.h — the guest bytes a run overwrote and what they held, so the run can be undone.
#pragma once

#include <cstddef>
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
    held_.clear();
    armed_ = true;
  }
  void note(std::uint8_t *at, std::uint32_t width) {
    entries_.push_back(Entry{.at = at, .width = width, .held = held_.size()});
    held_.insert(held_.end(), at, at + width);
  }
  void rollBack() {
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
      std::memcpy(entry->at, held_.data() + entry->held, entry->width);
    }
    entries_.clear();
    held_.clear();
    armed_ = false;
  }

private:
  struct Entry {
    std::uint8_t *at = nullptr;
    std::uint32_t width = 0;
    std::size_t held = 0; // offset of the old bytes in held_
  };
  std::vector<Entry> entries_;
  std::vector<std::uint8_t> held_;
  bool armed_ = false;
};

} // namespace psx
