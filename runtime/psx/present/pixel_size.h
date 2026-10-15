// pixel_size.h — a width and height in pixels.
#pragma once

namespace psx::present {

struct PixelSize {
  int width = 0;
  int height = 0;

  bool operator==(const PixelSize &) const = default;
};

} // namespace psx::present
