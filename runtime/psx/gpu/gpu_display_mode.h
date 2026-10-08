// gpu_display_mode.h — pure decode of the PSX GP1(08h) display-mode register.
#pragma once

#include <cstdint>

// Horizontal resolution is one three-bit field split across GP1(08h): HRES2 at bit 6 selects the
// dedicated 368-dot mode; only when it is clear do HRES1:0 select 256/320/512/640. Treating bit 6 as
// documentation but not data silently reports 368-dot title geometry as 256 dots.
inline int gp1_display_width(uint32_t mode) {
  if ((mode & (1u << 6)) != 0) {
    return 368;
  }
  constexpr int kWidths[] = {256, 320, 512, 640};
  return kWidths[mode & 3u];
}
