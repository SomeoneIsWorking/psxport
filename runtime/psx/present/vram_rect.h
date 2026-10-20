// vram_rect.h — VRAM's size and a position and rectangle in it, in native pixels.
#pragma once

namespace psx::gpu {

inline constexpr int kVramWidth = 1024;
inline constexpr int kVramHeight = 512;

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
