#pragma once

#include <cstdint>
#include <vector>

namespace psx::host {

// A title's logo as straight-alpha RGBA8, `width * height * 4` bytes.
struct LogoImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
};

} // namespace psx::host
