// vram_rect.h — a VRAM position and an origin-and-size VRAM rectangle, in native pixels.
#pragma once

namespace psx::gpu {

struct VramPoint {
  int x = 0;
  int y = 0;
};

struct VramRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

} // namespace psx::gpu
