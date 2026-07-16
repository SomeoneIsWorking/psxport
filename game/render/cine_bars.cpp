// game/render/cine_bars.cpp — native producer for the CINEMATIC LETTERBOX bars.
//
// Cutscenes draw top+bottom black letterbox bars via a UI-effect MANAGER: FUN_80026368 walks 8 effect
// slots (base 0x80100400, stride 0x4C), dispatching each active slot by its type byte (+2) through the
// table @0x8009D314. Slot type 1 = LETTERBOX (handler FUN_80026864): it grows/holds/shrinks a bar height
// h (slot+8) and each frame draws two op-0x60 FLAT black rects — top (0,0,320,h) and bottom (0,224-h,
// 320,h) — into the near OT bucket. The native object walk never drew these (the slots aren't render
// nodes), so cutscenes under pc_render were missing their bars (the field showed through top/bottom).
//
// RE (2026-07-16, Ghidra on the cutscene dump scratch/bin/ram_cut.bin):
//   FUN_80026864(slot): if scratchpad *(u8*)0x1F80019A != 2 -> return (manager not armed).
//     state = slot+4: 0 -> arm (no draw); 1 -> h++ (until >11) then draw; 2 -> hold, draw;
//     3 -> h-- then draw. States 1/2/3 draw; 0 doesn't. h = *(s16*)(slot+8).
//     draw = FUN_8007fcc8(0,0,320,h,1) + FUN_8007fcc8(0,224-h,320,h,1) — op-0x60 flat rects, colour 0,
//     opaque, near bucket. Verified: h=12 -> bars rows 0..11 + 212..223, byte-matching psx.
//
// READ-ONLY: the substrate manager still runs underneath (it owns the h tick + the guest packets); this
// producer only READS the slot table and re-emits the bars to the native queue.
#include "core.h"
#include "game.h"
#include "render.h"
#include "render_queue.h"

// cineBarsRender — emit any active cinematic letterbox bars. Emits nothing when the manager is disarmed
// (scratchpad 0x1F80019A != 2) or no letterbox slot is active. Call from any scene that can be a cutscene.
void Render::cineBarsRender() {
  Core* c = mCore;
  if (c->mem_r8(0x1F80019Au) != 2) return;               // manager not armed -> no bars
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  for (int i = 0; i < 8; i++) {
    const uint32_t slot = 0x80100400u + (uint32_t)i * 0x4Cu;
    if (c->mem_r8(slot) == 0) continue;                  // inactive slot
    if (c->mem_r8(slot + 2) != 1) continue;              // type 1 == letterbox
    const uint8_t state = c->mem_r8(slot + 4);
    if (state < 1 || state > 3) continue;                // states 1/2/3 draw; 0 = armed-not-drawing
    const int h = (int16_t)c->mem_r16(slot + 8);
    if (h <= 0) continue;
    // VISUAL FIX (USER 2026-07-16: "problems with letterbox even in the oracle — just make them look
    // similar, don't rely on the oracle"). The guest math frames the bars for a 224-line display (top
    // inner edge at h, bottom inner edge at 224-h), but we present the full 240-line framebuffer — so the
    // oracle's bar leaves a visible 16px strip of CONTENT below the bottom bar (rows 224..239). Keep the
    // INNER edges exactly where the game frames the content (top=h, bottom=224-h — the cinematic frame),
    // but extend each bar's OUTER edge flush to the real frame edge (top->0, bottom->FB_H) so the bars sit
    // against the screen edges with no floating gap. FB_H = the presented framebuffer height.
    const int FB_H = 240;
    auto bar = [&](int yTop, int yBot) {                     // filled [yTop, yBot) in screen rows
      const int bh = yBot - yTop; if (bh <= 0) return;
      int xs[4] = { 0 + ox, 320 + ox, 0 + ox, 320 + ox };
      int ys[4] = { yTop + oy, yTop + oy, yBot + oy, yBot + oy };
      int z[4] = { 0, 0, 0, 0 }; unsigned char k[4] = { 0, 0, 0, 0 };
      c->game->activeRq().push2dQuad(RQ_OVERLAY, /*order_2d_fg=*/1, xs, ys, z, z, k, k, k,
                                     0, 0, /*mode=*/3, /*raw=*/0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 511);
    };
    bar(0, h);              // top:    outer edge 0 (flush) .. inner edge h (game's content frame)
    bar(224 - h, FB_H);     // bottom: inner edge 224-h (game's content frame) .. outer edge FB_H (flush)
  }
}
