// The frames a diagnostic knob names, on whichever frame axis its owner counts.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace psx::debug {

class FrameSchedule {
public:
  FrameSchedule() = default;
  explicit FrameSchedule(std::vector<std::uint32_t> frames) : frames_(std::move(frames)) {
    std::ranges::sort(frames_);
  }

  [[nodiscard]] bool contains(std::uint32_t frame) const {
    return std::ranges::binary_search(frames_, frame);
  }
  [[nodiscard]] bool empty() const {
    return frames_.empty();
  }
  [[nodiscard]] std::size_t size() const {
    return frames_.size();
  }

private:
  std::vector<std::uint32_t> frames_;
};

} // namespace psx::debug
