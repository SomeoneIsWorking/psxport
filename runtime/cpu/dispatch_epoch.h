#pragma once

#include <cstdint>

namespace psx::cpu {

// The version of every input a dispatch verdict is derived from: which code images are resident and which bytes
// each owns, which title overrides are installed, which platform HLE leaves are registered,
// and the Game whose service table answers. Each input's owner advances it where it mutates, so a verdict learns it
// is stale from one compare. A suppression moves one address's verdict, which NativeDispatcher drops alone.
class DispatchEpoch {
public:
  void advance() {
    ++value_;
  }
  [[nodiscard]] std::uint64_t value() const {
    return value_;
  }

private:
  std::uint64_t value_ = 1; // 0 marks an empty verdict slot
};

} // namespace psx::cpu
