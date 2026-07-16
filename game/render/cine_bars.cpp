// game/render/cine_bars.cpp — CUSTOM PC-NATIVE cinematic LETTERBOX bars.
//
// Cutscenes flag a letterbox via a UI-effect MANAGER: FUN_80026368 walks 8 effect slots (base
// 0x80100400, stride 0x4C); slot type 1 = LETTERBOX (handler FUN_80026864). The guest handler grows/
// holds/shrinks a bar-height h (slot+8) and draws two op-0x60 flat black rects sized for a 320x224 PSX
// display. The slots aren't render nodes, so the native object walk never drew them.
//
// This producer does NOT transcribe the PSX draw (USER 2026-07-16: "I don't really care about the
// letterbox — we should have it but it should be custom PC-native so it can be adjusted for wide and 60").
// It uses the guest slot ONLY as the SIGNAL: is a letterbox active, and how far grown (progress fraction).
// The bars themselves are a native overlay sized to the ACTUAL display, so they adapt to any aspect and
// present rate:
//   - WIDE: bars span the full render-target width (drawn far past any aspect's extent; the target/draw-
//     area clip trims the overdraw), so 16:9 / 21:9 margins are covered — a 320-wide bar would leak the
//     side content at top/bottom.
//   - 60fps: RQ_OVERLAY re-emits every presented frame; the progress fraction is read live, so an fps60
//     tier that lerps the guest h would animate the bars smoothly with no extra work here (the one knob
//     is `progress`).
// The bar THICKNESS keeps the game's cinematic framing (so the cutscene composition isn't cropped): the
// full-grown bar matches the guest's — h/kGuestFullH of the frame at each edge. READ-ONLY (the substrate
// manager still runs underneath and owns the guest packets; this only reads the slot table).
#include "core.h"
#include "game.h"
#include "render.h"
#include "render_queue.h"

// Wide-render-target width (428 @16:9, 560 @21:9, 320 @4:3); ofx used elsewhere. Declared in gpu_gpu.cpp.
int gpu_gpu_wide_engine(Core* c);
int gpu_gpu_wide_engine_w(Core* c);

// cineBarsRender — emit the cinematic letterbox as a native full-width overlay. Emits nothing when no
// letterbox is armed. Call from any cutscene-capable scene.
void Render::cineBarsRender() {
  Core* c = mCore;
  if (c->mem_r8(0x1F80019Au) != 2) return;               // UI-effect manager not armed -> no bars

  // Find the active letterbox slot's progress (0..1). The guest grows h to kGuestFullH over its intro.
  constexpr int kGuestFullH = 12;                        // guest bar height at full (FUN_80026864 caps h>11)
  float progress = 0.0f;
  for (int i = 0; i < 8; i++) {
    const uint32_t slot = 0x80100400u + (uint32_t)i * 0x4Cu;
    if (c->mem_r8(slot) == 0 || c->mem_r8(slot + 2) != 1) continue;   // active + type-1 (letterbox)
    const uint8_t state = c->mem_r8(slot + 4);
    if (state < 1 || state > 3) continue;                            // 1/2/3 draw; 0 armed-not-drawing
    const int h = (int16_t)c->mem_r16(slot + 8);
    if (h > 0) { float p = (float)h / (float)kGuestFullH; progress = p > progress ? p : progress; }
  }
  if (progress <= 0.0f) return;
  if (progress > 1.0f) progress = 1.0f;

  // Native bar geometry, sized to the DISPLAY (not the PSX 320x224). Thickness = the guest's cinematic
  // frame fraction (kGuestFullH of 240) scaled by progress, applied symmetrically at top and bottom.
  const int H = 240;                                     // present framebuffer height (native draw units)
  const int barPx = (int)(progress * (float)kGuestFullH + 0.5f);
  if (barPx <= 0) return;

  // Full-width span: draw far beyond any aspect's extent and let the target/draw-area clip trim it. The
  // 2D origin sits at the 4:3 left edge with the wide content extending both ways, so cover -X..+X wide
  // enough for the widest target (wide_w up to VRAM_W). ±((wide_w-320)/2 + 320) always spans it.
  const int wide_w = gpu_gpu_wide_engine(c) ? gpu_gpu_wide_engine_w(c) : 320;
  const int margin = (wide_w - 320) / 2;
  const int xL = -margin - 320, xR = 320 + margin + 320; // generous overdraw; clipped to the target
  const int oy = c->game->gpu.s_off_y;

  auto bar = [&](int yTop, int yBot) {
    if (yBot <= yTop) return;
    int xs[4] = { xL, xR, xL, xR };
    int ys[4] = { yTop + oy, yTop + oy, yBot + oy, yBot + oy };
    int z[4] = { 0, 0, 0, 0 }; unsigned char k[4] = { 0, 0, 0, 0 };
    c->game->activeRq().push2dQuad(RQ_OVERLAY, /*order_2d_fg=*/1, xs, ys, z, z, k, k, k,
                                   0, 0, /*mode=*/3, /*raw=*/0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 511);
  };
  bar(0, barPx);             // top bar, flush to the top edge
  bar(H - barPx, H);         // bottom bar, flush to the bottom edge (symmetric — no floating gap)
}
