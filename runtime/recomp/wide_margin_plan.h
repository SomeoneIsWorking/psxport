#pragma once

// Renderer-only base coverage for storage added to the RIGHT of a guest framebuffer. PSX games use
// that VRAM for textures/CLUTs, so the extension must be covered in the host composite without ever
// clearing guest VRAM. The native framebuffer itself is deliberately excluded: preserve-backdrop
// ports need its uploaded pixels intact.
struct WideMarginPlan {
  bool draw = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline WideMarginPlan plan_wide_margin(int sx, int sy, int native_w, int wide_w, int h) {
  WideMarginPlan p{};
  if (native_w <= 0 || wide_w <= native_w || h <= 0) return p;
  p.draw = true;
  p.x0 = sx + native_w;
  p.y0 = sy;
  p.x1 = sx + wide_w;
  p.y1 = sy + h;
  return p;
}
