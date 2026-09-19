#include "gpu_vk_internal.h"
#include "wide_margin_plan.h"

// WIDESCREEN STORAGE IS NOT A FRAMEBUFFER. The guest owns only [sx,sx+native_w); the extra
// host-visible columns out to disp_w are ordinary PSX VRAM and commonly hold textures/CLUTs. Loading
// that region into the persistent composite leaks atlas pixels wherever no later primitive covers
// it, so an opaque black base goes behind the extension in the 2D-background band. This changes only
// the host render batch: guest VRAM remains byte-for-byte intact, and authored backdrop/world/HUD
// geometry draws over it in the normal three-band order. Index 0 is the back of the background band,
// so this cannot cover an authored background primitive.
void GpuVkState::draw_wide_margin(int sx, int sy, int native_w, int disp_w, int h, bool rgb24) {
  const WideMarginPlan margin = plan_wide_margin(sx, sy, native_w, disp_w, h, rgb24);
  if (!margin.draw) {
    return;
  }
  set_order_2d_bg(0);
  // FULL CANVAS clip, deliberately: the wide margin exists to paint the strip OUTSIDE the guest's own
  // draw area, so clipping it to that area would erase exactly what it is for.
  draw_tri(
      margin.x0, margin.y0, 0, 0, 0, margin.x1, margin.y0, 0, 0, 0, margin.x0, margin.y1, 0, 0, 0, 0, 0, 1023, 511);
  draw_tri(
      margin.x1, margin.y0, 0, 0, 0, margin.x1, margin.y1, 0, 0, 0, margin.x0, margin.y1, 0, 0, 0, 0, 0, 1023, 511);
}
