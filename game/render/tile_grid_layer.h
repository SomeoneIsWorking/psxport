// game/render/tile_grid_layer.h — PC-native (dual-emit) body for the field's scrolling 16x16
// tile-grid background: the scroll-wrap helper FUN_8011534C and the op-0x7C sprite-packet emitter
// FUN_80115598 (A00 overlay, guest node @ 0x800ED018). See tile_grid_layer.cpp for the full RE +
// entry-point-resolution trace (docs/native-render-2d-tilegrid.md).
//
// NOT the same call chain as the RE doc originally guessed: docs/native-render-2d-tilegrid.md's
// "Function boundaries" section names 0x80115364 (Render::overlayTypeDispatch case 0x8003D1C4) as
// the dispatch entry. That address is NOT a real function entry (see .cpp "entry resolution") and
// that call path is dead for the field. The REAL, live callers are:
//   - Engine::areaModeDispatch(Faithful) (game/core/engine.cpp, mode idx 0) -> rec_dispatch(c,
//     0x8011534Cu) with a0=0x800ED018 — per-frame STEP (scroll wrap).
//   - the still-unowned FUN_8003DF04 state dispatcher -> rec_dispatch(c, 0x80115598u) with
//     a0=0x800ED018 for render-state 0 — per-frame EMIT.
// This class owns those TWO leaf bodies directly (wired via engine_set_override_a00, the SAME
// mechanism OverlayGroundGt3Gt4 uses for its A00-local leaves — neither is reached via rec_dispatch
// from a literal MAIN jal, both are reached through per-node function-pointer / table dispatch that
// resolves to these two addresses at runtime).
//
// HOST HALF (pc_render): Render::backdropRender (game/render/render_walk.cpp) is the ALREADY-OWNED,
// unconditional native producer for this exact tile grid (transcribes the same wrap/index math,
// draws every frame regardless of guest state). This class's OWN host-side quad push is therefore
// gated OFF during normal play (oracle/psxRender only — see .cpp) so pc_render is never double-fed;
// it exists purely so the psx_render/oracle legs (visual A/B, SBS-adjacent tooling) see the same
// picture the guest packets would produce.
#pragma once
struct Core;
class  Game;

class TileGridLayer {
public:
  static void scrollStep(Core* c);  // FUN_8011534C(node=a0) — state 0: one-time init from
                                     // *0x800ECF84; state 1: recompute wrapped scroll X/Y into
                                     // node+0x28/+0x2A from scratchpad 0x1F8000F2/F0.
  static void emit(Core* c);        // FUN_80115598(node=a0) — walk the W×H tile grid, emit one
                                     // 16-byte op-0x7C sprite packet per visible tile into the
                                     // shared packet pool (0x800BF544), splice into OT bucket
                                     // 0x7FF, append a trailing DR_TPAGE reset packet via the
                                     // already-RE'd func_80083DE0.

  static void registerOverrides(Game* game);
};
