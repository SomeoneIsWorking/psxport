// class Panel — native 2D UI panel family: substrate-mirror TAPS on the two live panel builders.
//
// Guest fns owned (docs/native-render-2d-panel.md Spec 1 + Spec 2, RE'd 2026-07-15):
//   FUN_8005019C  panelBuild(rect* r4, u16 style r5, u8 shadow r6, int otBucket r7)  — 9-slice panel.
//   FUN_8004FFB4  panelFill (rect* r4, int uvIndex r5, u16 attr r6, int otBucket r7) — one FT4 fill,
//                 LEAF (called 5x by panelBuild for top/bottom/left/right/center).
//
// TAP ARCHITECTURE (engine-overrides directive, same shape as ScreenFade::installLeafTap in
// game/render/screen_fade.cpp/.h — read that first): each guest builder gets an override installed
// via `engine_set_override_main(addr, tap, gen_func_addr)`. A tap (a) captures the guest-ABI args
// from c->r[4..7] at entry, (b) calls the ORIGINAL gen_func_XXXX(c) so every guest byte (packet
// pool, OT, stack) stays byte-exact — SBS core B never sees this table (oracle-gated thunk), so it
// keeps running pure gen — then (c) pushes the equivalent native quads to `c->game->activeRq()`,
// gated on `c->game->oracle || c->rsub.mode.psxRender()` so pc_render stays a READ-ONLY overlay
// (host memory only, zero guest writes, zero c->r[] writes beyond what gen already left).
//
// NESTING: gen_func_8005019C's guest body calls panelFill through the WRAPPER `func_8004FFB4`
// (the g_override[]-checking dispatch thunk), not gen_func_8004FFB4 directly (confirmed by grepping
// generated/shard_3.c — the panelBuild body's 5 call sites all say `func_8004FFB4(c)`). So once
// panelFill's tap is installed, panelBuild's gen call automatically nests through it and the 5 fills
// are pushed by the panelFill tap — panelBuild's own tap only needs to push its 4 corner sprites.
#pragma once
#include <cstdint>
class Core;

class Panel {
public:
  // install(): registers both taps on the shard override table. Idempotent; call once from
  // games_tomba2_init alongside the other *_install()/installLeafTap() wirings.
  static void install();

  // pushFill: native half of the panelFill tap (Spec 2) — one textured FT4 quad over `rectPtr`'s
  // rect (4 s16 at guest ptr: x,y,w,h), UV selected by `uvIndex` (0..4, the spec's texel table).
  static void pushFill(Core* c, uint32_t rectPtr, int32_t uvIndex, uint16_t attr, int32_t otBucket);

  // pushDialogGlyphs: native half of the FUN_8007CC00 tap (Spec 3) — the dialog box's per-glyph
  // text row, including the selected-option HIGHLIGHT palette (box+0x47/box+3 -> clut 0x7CBE) the
  // retired flat-list producer (Render::dialogTextNative) couldn't see. See panel.cpp.
  static void pushDialogGlyphs(Core* c, uint32_t box);

  // pushCorners: native half of the panelBuild tap (Spec 1) — the 4 corner 8x8 SPRT_8 sprites
  // (TL/TR/BL/BR). Does NOT push the 5 fills — those arrive via panelFill's own tap (see NESTING
  // note above).
  static void pushCorners(Core* c, uint32_t rectPtr, uint16_t style, uint32_t shadow, int32_t otBucket);
};
