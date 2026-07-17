// game/render/fps60_worldpass.cpp — TRANSITIONAL fps60 world-pass seam (see game_iface.h death condition).
//
// The interpolated-60fps lerp tier (Fps60) is a GENERIC renderer feature and lives framework-side
// (runtime/recomp/fps60.cpp). Its interp present RE-RUNS the game's world passes one frame behind, under
// the framework Fps60's lerped inputs, into its isolated sink. That re-run is the ONE place the framework
// still reaches into game render — carried here, behind the fps60WorldPass / fps60BbSwapPrev hooks.
//
// TARGET (USER 2026-07-17): the game submits its drawables to the framework once and the framework lerps
// them directly — no callback into game render. Delete this file + both hooks when that submit model lands.
//
// The projParams/rqRedirect/camera-override/DisplayPassGuard scaffold stays framework-side (Fps60::
// tier1Render); this body is only the gate reads + the world-pass draws, plus the backdrop wrap-lerp that
// writes the framework Fps60's (public) bg-override state that Render::backdropRender reads back.
#include "core.h"
#include "game.h"        // c->game->fps60 — the framework Fps60 (public bg/obj-override state written here)
#include "game_ctx.h"    // rend(c) — the game's Render umbrella accessor
#include "render.h"      // Render::worldVoidBeat/fieldAreaInit/terrainRenderAll/fieldEntityRender/backdropRender/...
#include "fps60.h"       // Fps60 — the override struct fields
#include <math.h>

// wrapLerp — copy of the framework helper (fps60.cpp): shortest-signed-path lerp across a [0,mod) wrap
// (ParallaxBg::step's wrapMod). A naive prev+(cur-prev)*t sweeps the long way around a wrap boundary.
static int wrapLerp(int prev, int cur, int mod, float t) {
  if (mod <= 0) return prev + (int)lroundf((float)(cur - prev) * t);
  int diff = cur - prev;
  if (diff >  mod / 2) diff -= mod;
  if (diff < -mod / 2) diff += mod;
  int v = prev + (int)lroundf((float)diff * t);
  v %= mod; if (v < 0) v += mod;
  return v;
}

void tomba_fps60_world_pass(Core* c, float t) {
  Fps60& f = c->game->fps60;
  // #67 GATE PARITY: mirror the REAL frame's world-pass gates (Render::worldVoidBeat / fieldAreaInit —
  // the same reads sceneNative made this interval). Re-running past a gate the real frame honored paints
  // that pass on interp presents only (30Hz flicker of the whole layer).
  const bool voidBeat = rend(c)->worldVoidBeat();
  const bool areaInit = rend(c)->fieldAreaInit();
  if (!voidBeat && !areaInit) rend(c)->terrainRenderAll();
  // SCENE TABLE (grass/terrain props): camera-only, same gate as terrain (mSceneTableTrusted, per the
  // present-time invariant — no tick has run since the real frame computed it).
  if (!voidBeat && !areaInit && rend(c)->mSceneTableTrusted) rend(c)->fieldEntityRender(0x800F2418u);
  // BACKDROP (game-logic scroll, LAYER-TRANSFORM lerp — not camera-projected): mirrors sceneNative's own
  // gate (mBackdropTrusted && the field-drawer-selector byte == 0 && bgstate == 0). The wrap moduli
  // (t4+0x30/+0x32) are static per-area config, safe to re-read directly here.
  if (!voidBeat && rend(c)->mBackdropTrusted && c->mem_r8(0x800bf873u) == 0 && c->mem_r8(0x800bf870u) == 0) {
    int modX = c->mem_r16(0x800ed018u + 0x30u), modY = c->mem_r16(0x800ed018u + 0x32u);
    f.mBgOverride.scrollX = wrapLerp(f.mBgPrev.scrollX, f.mBgCur.scrollX, modX, t);
    f.mBgOverride.scrollY = wrapLerp(f.mBgPrev.scrollY, f.mBgCur.scrollY, modY, t);
    f.mBgOverrideOn = true;
    rend(c)->backdropRender(0x800ed018u);
    f.mBgOverrideOn = false;
  }
  // Field OBJECT walk under lerped per-object transforms (mObjOverrideOn + the captured projObj) AND the
  // still-armed lerped camera into the sink. Objects run on the void beat (vortex node) like the real
  // frame; only areaInit suppresses them (mirrors sceneNative's field_area_init block).
  if (!areaInit) {
    f.mObjOverrideOn = true;
    rend(c)->fieldObjectsRender();
    f.mObjOverrideOn = false;
  }
}

void tomba_fps60_bb_swap_prev(Core* c) { rend(c)->bbSwapPrev(); }
