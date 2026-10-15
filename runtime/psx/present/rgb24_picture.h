// rgb24_picture.h — a host-side RGB picture read back from the device's display.
#pragma once

#include <cstdint>
#include <vector>

namespace psx::gpu {

struct Rgb24Picture {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels; // row-major RGB
};

} // namespace psx::gpu
