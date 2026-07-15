// NATIVE render-list WALK subsystem (Tomba2Engine) — the engine's PC-native SCENE RENDERER.
//
// Driven from the game's WORLD DATA (entity lists -> per-object geomblk node+0xC0 cmds), NOT from PSX GP0
// packets: ov_scene_native orchestrates the per-frame field render, and the master / snapshot / aux list
// walks (native twins of gen_func_8003C048 / 8003BB50 / the BCF4/BF00/EEC0 aux walks) iterate the engine's
// render lists, dispatching each live node by its render TYPE and tagging every prim with the object's
// PC-native world-position depth so 2D billboards occlude for real at the deferred OT walk. The per-object
// flush composes the float camera x object transform and submits generic GT3/GT4 natively (no PSX per-mode
// dispatcher), and the native backdrop draws the sky/parallax tilemap as RQ_BACKGROUND behind the world.
//
// Split out of submit.cpp (the geometry-SUBMIT subsystem) so the scene renderer is its own PC-game
// file. Shared helpers (PktSpanSession, obj_world_ord/cur_render_node) live in render_internal.h.
#include "core.h"
#include "render.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include "render_queue.h"
#include "projection.h"   // EObjXform (per-object world-coord float projection; ops on Render)
#include "render_internal.h"
#include "player/actor_tomba.h"   // ActorTomba::G_ADDR — Tomba's node, outside the 3 generic entity lists
#include <stdio.h>
#include <string.h>
#include <math.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);
int  rec_addr_has_entry(Core*, uint32_t);   // overlay_router.cpp — is fn a real entry in the resident module?
#define OTBASE_PTR   0x800ED8C8u             // *this = the active ordering-table base
#define SCR          0x1F800000u             // PSX scratchpad base (the engine's GTE-compose temp area)

// The per-object render path is FULLY native (no PSX fallback): submit_perobj_flush composes the float
// world transform and calls Render::gt3gt4 directly. The old gen_func_8003F698 per-mode dispatcher (which
// ran interpreted per-scene submitter variants for non-generic modes) is no longer consulted — every
// per-object geomblk is submitted as generic GT3/GT4 through the native, world-coord projection.

// NATIVE-DRAWN-NODE provenance (bug #48, render.h banner). ORDERING: the substrate walk cluster
// (Render::frame -> ... -> perObjRenderDispatch -> cmdListDispatch, the reader below) runs from
// Engine::fieldFrame EARLIER in the SAME logic frame than Engine::drawOTag -> sceneNative ->
// perObjFlush (the native draw, engine.cpp fieldFrame calls mRender->frame() at the render-submit
// step; drawOTag runs later, from native_boot's own post-scheduler walk) — so a registry perObjFlush
// WRITES cannot be read by cmdListDispatch in the same frame; it would always see last frame's (or an
// empty) set. Rather than accept that one-frame lag (which would transiently mis-cover or
// mis-duplicate on the exact frame an object's cull/render-list state changes), this queries the
// GUEST DATA directly with the IDENTICAL inclusion test Render::sceneNative's own object loop applies
// before calling perObjFlush (render_walk.cpp, HEADS[3] walk: live marker node+1, render-command
// counts node+8/+9) — so the set this computes is EXACTLY the set perObjFlush will visit this frame,
// derived from the same already-stable guest state (game logic for this frame has already run by the
// time either pass reads it), not a heuristic. Cached per s_frame (computed once, lazily, on first
// query) so repeated per-cmd queries in cmdListDispatch's loop are O(1) after the first.
bool Render::nativeObjDrawn(Core* c, uint32_t node) {
  const int frame = c->game->gpu.s_frame;
  if (mNativeDrawnFrame != frame) {
    mNativeDrawnNodes.clear();
    mNativeDrawnFrame = frame;
    static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
    for (int h = 0; h < 3; h++) {
      uint32_t n = c->mem_r32(HEADS[h]);
      for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
        if (c->mem_r8(n + 1) == 0) continue;                            // dead / not-live this frame
        if (c->mem_r8(n + 8) == 0 || c->mem_r8(n + 9) == 0) continue;   // no render commands
        mNativeDrawnNodes.insert(n);
      }
    }
  }
  return mNativeDrawnNodes.count(node) != 0;
}

void Render::perObjFlush() {
  Core* c = mCore;
  uint32_t node = c->r[4];
  if (c->mem_r8(node + 8) == 0) return;
  if (c->mem_r8(node + 9) == 0) return;
  // NOTE (bug #48): this node's cmd list (node+8 count, node+0xC0 array, geomblk=cmd+0x40) is drawn
  // natively below via gt3gt4 — the SAME array cmdListDispatch's substrate mirror walks for whichever
  // walker (perObjRenderDispatch) reaches this node. cmdListDispatch's coverage decision does NOT
  // depend on this call having run (the substrate walk runs BEFORE this display pass in the same
  // logic frame) — it queries Render::nativeObjDrawn, which re-derives this same node set straight
  // from guest state instead. See nativeObjDrawn's banner above.
  uint32_t otbase_ptr = c->mem_r32(OTBASE_PTR);              // *0x800ED8C8
  int i = 0;
  while (i < (int)c->mem_r8(node + 8)) {
    uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
    uint32_t geomblk = c->mem_r32(cmd + 0x40);
    if (geomblk != 0) {
      // PC-NATIVE: compose the camera × object transform in FLOAT from the object's REAL WORLD coordinates
      // (its world matrix cmd+0x18 + world position cmd+0x2c, transformed by the scene camera) and make it
      // the ACTIVE projection. The submitters project every vertex through it — NO gte_op, NO CR0-7.
      EObjXform w; c->mRender->projComposeObject(cmd, &w);
      c->mRender->projSetActive(&w);
      // OT base: node[0xd]&0xf == 4 selects a per-command sub-bucket (cmd[0x3f]*4 offset), else the base.
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4)
        otbase = otbase_ptr + ((c->mem_r8s(cmd + 0x3F)) << 2);
      // fps60 TRUE per-object tier: the object's world transform was captured (keyed by cmd) inside
      // projComposeObject above; the GT3/GT4 submit projects it. No per-prim key needed anymore.
      c->mRender->gt3gt4(geomblk, otbase);              // fully-native generic GT3/GT4 submit (no PSX fallback)
      c->mRender->projClearActive();
    }
    i++;
    if (i >= (int)c->mem_r8(node + 9)) break;
  }
}

// ===================================================================================================
// ONE NATIVE RENDER PATH — world-data-driven scene render (Phase 1, user 2026-06-24 architecture:
// [[one-native-render-path-decoupled]]). Driven from the GAME's WORLD DATA, NOT from PSX GP0 packets:
// walk the 3 active entity lists, and render each live object's 3D model (geomblk via node+0xC0 cmds)
// through the native float-projection submitters (eproj + D32 depth + engine lighting). This is the
// single mechanism depth/60fps/ires/lighting attach to. It runs as its OWN pass (not bolted onto the
// PSX OT-walk) so the draw state is the native pass's. Gated `debug scenenative` while standing it up.
// (g_scene_native_diag was defined here but never read; dead — removed 2026-07-02)
// g_sn_objs/g_sn_cmds retired 2026-07-03 — Render::stats.snObjs/snCmds (RenderStats).
// NATIVE BACKDROP tilemap drawer — overlay FUN_80115598 (the seaside field's state-0 background drawer,
// reached via 0x8003df04's 16-state jump table @0x80014fc0; state 0 → 0x8003df74 → 0x80115598). This is the
// sky + distant parallax hills (the only thing the decoupled native scene was missing — verified by SKIPPASS
// attribution, later-244). The PSX body reads the area's tile MAP (W×H grid of u16 tile entries) and a
// scroll position, then builds GP0 textured-sprite (cmd 0x7d) packets into the OT for the visible wraparound
// window of 16×16 tiles. We TRANSCRIBE the integer wrap/scroll/index math (that's scene data — what tile
// goes where), but emit NATIVE RQ_BACKGROUND 2D quads instead of GP0 packets / OT links (the engine owns the
// background layer; sky/hills sit behind the real-depth world). Struct @t4 (=0x800ed018 at the seaside):
//   +0x04 hword tpage  +0x06 hword clut-base  +0x10 byte W  +0x11 byte H
//   +0x14 word  tilemap ptr (u16[H][W])       +0x28 hword scrollX  +0x2a hword scrollY
// Tile entry bits: [0:3]=atlas col, [4:7]=atlas row, [8:11]=clut sub-index. U=(t&0xF)<<4, V=(t&0xF0)+8 (the
// half-tile V bias is in the source data layout — faithful), clut=clutbase+((t&0xF00)>>2). Texpage 0x0E =
// 4bpp @ VRAM(896,0) (set once via a GP0(0xE1) prim in the PSX body; applied per-quad here).
// TIER 1 BACKDROP (docs/fps60-rework.md): scrollX/scrollY are the ONLY per-frame-varying fields this fn
// reads (PARALLAX_BG_SM+0x28/+0x2A, computed by ParallaxBg::step from camera yaw/pitch every RUNNING
// tick) — everything else (W/H/tilemap ptr/tpage/clutbase/wrap-moduli) is static per-area config, set
// once at INIT and unchanged while running. The scroll read goes through the fps60 provider (mirrors
// sceneCam): byte-identical to the plain struct read when fps60 is off or this is the real per-logic-
// frame call (which also captures the result into Fps60::mBgCur); during Tier-1's present-time backdrop
// re-render (Fps60::tier1Render, fps60.cpp) it instead returns wrapLerp(mBgPrev,mBgCur,t), no guest read.
void Render::backdropRender(uint32_t t4) {
  Core* c = mCore;
  int W = c->mem_r8(t4 + 0x10), H = c->mem_r8(t4 + 0x11);
  if (W == 0 || H == 0) return;
  int rowstride = W * 2;                          // s0 — bytes per map row
  int mapbytes  = rowstride * H;                  // s3 — total map bytes (wrap modulus)
  int scrollX, scrollY;
  c->game->fps60.bgScroll(c, t4, scrollX, scrollY);
  uint32_t map      = c->mem_r32(t4 + 0x14);
  uint16_t tpage    = c->mem_r16(t4 + 0x04);
  uint16_t clutbase = c->mem_r16(t4 + 0x06);
  int tp_x = (tpage & 0xF) * 64, tp_y = ((tpage >> 4) & 1) * 256;
  int mode = (tpage >> 7) & 3; if (mode > 2) mode = 2;
  // Publish THIS frame's backdrop texpage so the OT walk can recognize the GUEST background drawer's
  // redundant sky/sea tiles (FUN_80115598) and drop them on the field: the native backdrop (this fn) already
  // owns the sky/sea as RQ_BACKGROUND. Replaces the dead ov_bg_tilemap packet-span provenance (orphaned by
  // the override-system removal 2026-06-22), which is why the redundant tiles regressed to RQ_HUD and
  // occluded the world (render.md OPEN #1). Data-derived (real backdrop tpage), not a magic constant.
  void gpu_bg_texpage_set(Core*, int, int); gpu_bg_texpage_set(c, tp_x, tp_y);
  // WIDESCREEN backdrop coverage (root-cause fix for the [320,nw) atlas-garbage band): the PSX body tiles
  // a 320-wide window centred at screen-x 160 (t5 = ...+0x160 = 352 = 320+32 slack). At a wide aspect the
  // engine projects the world with OFX=nw/2, so the visible field spans [0,nw) — but the sky/parallax
  // backdrop, drawn only across ~[0,344), left the right margin uncovered, exposing the raw VRAM texture
  // atlas that lives past the 320-wide FB (later-55 VRAM packing). Re-centre the backdrop on the wide
  // centre (cx=nw/2) and widen the tiled window to nw+32 so it fills the full wide FB, matching the
  // world's OFX shift. cx/winw reduce to the exact 4:3 values (160 / 0x160) when not wide, so the 4:3
  // path stays byte-identical. Gated on gpu_gpu_wide_engine() (false at 4:3 / oracle / SBS legs).
  int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
  int cx = 160, winw = 0x160;                       // screen-centre X / tiled window width (4:3 defaults)
  if (gpu_gpu_wide_engine(c)) { int nw = gpu_gpu_wide_engine_w(c); cx = nw / 2; winw = nw + 0x20; }
  // Starting tile row/col = (scroll - screen-center) >> 4, wrapped into [0,H) / [0,W).
  int rowtile = ((scrollY - 120) >> 4) % H; if (rowtile < 0) rowtile += H;
  int coltile = ((scrollX - cx) >> 4) % W; if (coltile < 0) coltile += W;
  int t2 = rowtile * rowstride;                   // current row byte offset (wraps mod mapbytes)
  int coloff0 = coltile * 2;                       // starting col byte offset (wraps mod rowstride)
  int xoff = (int16_t)(cx - 8 - scrollX);          // t9 — sub-tile X scroll remainder + screen offset
  int yoff = (int16_t)(112 - scrollY);             // s7 — sub-tile Y scroll remainder + screen offset
  unsigned char col[4] = { 0x80, 0x80, 0x80, 0x80 };  // 0x7d is raw-texture: color ignored
  int outer_bound = (int16_t)(scrollY - 120) + 0x100; // 16 rows
  int t5 = (int16_t)(scrollX - cx) + winw;            // wide-covering column window (4:3: ~22 cols)
  // Tag every backdrop tile with the reserved kBackdropDbgNode sentinel (render_queue.h) — NOT the
  // dbg_node==0 a generic OT-walk-classified RQ_BACKGROUND item gets (menu backdrop art, hut-interior
  // clear, SOP fills). This is what lets Fps60::tier1Render's queue-lerp exclusion (fps60.cpp
  // isTier1Owned) target ONLY the prims it actually re-renders, same pattern as terrain/scene-table (#54).
  c->mRender->diag.beginObject(kBackdropDbgNode);
  for (int t8 = scrollY - 120;;) {
    int Y = (int16_t)((t8 & 0xFFF0) + yoff);
    int t6 = (int16_t)t2;                          // row byte offset (sign-extended)
    int t0 = coloff0;
    for (int t1 = scrollX - cx;;) {
      int X = (int16_t)((t1 & 0xFFF0) + xoff);
      uint16_t tile = c->mem_r16(map + (uint32_t)(t6 + t0));
      int u = (tile & 0xF) << 4, v = (tile & 0xF0) + 8;
      uint16_t clut = (uint16_t)(clutbase + ((tile & 0xF00) >> 2));
      int clut_x = (clut & 0x3F) * 16, clut_y = (clut >> 6) & 0x1FF;
      int xs[4] = { X, X + 16, X, X + 16 }, ys[4] = { Y, Y, Y + 16, Y + 16 };
      int us[4] = { u, u + 16, u, u + 16 }, vs[4] = { v, v, v + 16, v + 16 };
      sil_bbox_log_i("bg_tilemap", xs, ys, 4);
      // Tier-1 redirect (mirrors native_terrain.cpp / fieldEntityRender's fix — see fps60-rework.md
      // "Tier 1 extended"): route through rqRedirect so re-invoking this fn at present time (Fps60::
      // tier1Render) lands in the isolated mSink, never the live queue the next real frame will build.
      (c->game->rqRedirect ? *c->game->rqRedirect : c->game->rq)
          .push2dQuad(RQ_BACKGROUND, /*order_2d_fg=*/0, xs, ys, us, vs, col, col, col,
                             tp_x, tp_y, mode, /*raw=*/1, clut_x, clut_y, 0, 0, 0, 0, 0, 0, 1023, 511);
      c->mRender->stats.snCmds++;
      t0 += 2; if (t0 >= rowstride) t0 = 0;        // column wrap
      t1 += 16;
      if (!((int16_t)t1 < t5)) break;
    }
    t2 += rowstride; if ((int16_t)t2 >= mapbytes) t2 -= mapbytes;  // row wrap
    t8 += 16;
    if (!((int16_t)t8 < outer_bound)) break;
  }
  c->mRender->diag.endObject();
}

// ---- pc_render scene DISPATCH (see render.h) --------------------------------------------------------
// Classify the current scene from the resident stage (0x801FE00C) + its sub-state selectors.
Render::SceneKind Render::classifyScene() {
  Core* c = mCore;
  const uint32_t stage = c->mem_r32(0x801FE00Cu);
  if (stage == 0x8010649Cu) return SceneKind::StartBoot;     // START.BIN loader
  if (stage == 0x801062E4u) return SceneKind::Title;         // DEMO/title front-end (title + substates)
  if (stage == 0x8010637Cu) {                                // GAME field stage
    const uint32_t task_sm = c->mem_r32(0x1F800138u);
    if (task_sm && c->mem_r16(task_sm + 0x4Cu) == 3) return SceneKind::HutInterior;   // authored sub-scene
    if (c->mem_r32(0x80109450u) == 0x3C021F80u)      return SceneKind::SopNarration;  // SOP overlay loaded
    return SceneKind::Field;                                                          // walkable free-roam
  }
  return SceneKind::Unknown;
}

// renderScene — the ONE native-renderer dispatch. No guest-OT transcription; each producer builds the
// picture from game state and emits to the render queue. A stage with no producer aborts with its identity.
void Render::renderScene() {
  switch (classifyScene()) {
    case SceneKind::StartBoot:    renderStartBoot();    break;
    case SceneKind::Title:        renderTitle();        break;
    case SceneKind::Field:        renderField();        break;
    case SceneKind::HutInterior:  renderHutInterior();  break;
    case SceneKind::SopNarration: renderSopNarration(); break;
    case SceneKind::Unknown:
    default:
      mCore->game->fps60.mTier1EligibleCur = false;
      abortUnimplemented("stage with no native producer");
  }
}

// #1 START.BIN boot (0x8010649C): the loader shows a black screen (empty OT, FB mean 0) for ~5 frames while
// it builds the file table / preloads. Native producer = a black loading frame (the observable result).
void Render::renderStartBoot() {
  mCore->game->fps60.mTier1EligibleCur = false;
  mCore->game->gpu.gpu_blank_display();     // zero the display FB -> present shows black
}

// #2 DEMO/TITLE front-end (stage 0x801062E4). Substate s2 (sm[0x48]==2) = the static title (logo + New/Load
// menu + copyright) via titleNative (emits from source state). Other substates = the loading ramp / OP.STR
// movie / attract, FMV/CD states shown black headless (the movie is skipped) — the honest result.
void Render::renderTitle() {
  Core* c = mCore;
  c->game->fps60.mTier1EligibleCur = false;
  if (c->mem_r16(0x801FE048u) == 2) {
    DisplayPassGuard displayPass(c->mRender->mode);   // read-only: reads source state, emits host-only
    titleNative();
  } else {
    c->game->gpu.gpu_blank_display();                 // loading ramp / FMV-skipped movie/attract -> black
  }
}

// #3 WALKABLE FIELD — native WORLD: terrain + entity/scene tables + objects + backdrop, real per-pixel
// depth. The 2D layer (HUD/text/dialog/billboards) comes from its own native producers, not the OT.
void Render::renderField() {
  mCore->game->fps60.mTier1EligibleCur = true;   // native field render runs -> fps60 tier-1 may re-render it
  DisplayPassGuard displayPass(mCore->mRender->mode);   // read-only invariant: aborts on any guest write
  sceneNative();
  dialogTextNative();   // in-game dialog / prompt text (emits nothing when no dialog is up)
}

// #4 HUT/DOOR INTERIOR (task-sm[0x4c]==3): OBJECTS-ONLY. The room is entity-list object 0x800FD850
// (HEADS[1]); NPCs/props are HEADS[0..1]; Tomba is the G block — fieldObjectsRender walks them all via
// perObjFlush -> gt3gt4 with real depth + the live interior camera. Skips the exterior terrain/scene-table/
// backdrop (village data) — the substrate's reduced frameX pass. 2D bubble = native producer when rebuilt.
void Render::renderHutInterior() {
  mCore->game->fps60.mTier1EligibleCur = false;
  DisplayPassGuard displayPass(mCore->mRender->mode);
  fieldObjectsRender();
  dialogTextNative();   // interior dialog / prompt text (e.g. the "Use + to talk" bubble text)
}

// #5 SOP INTRO NARRATION (overlay-sig 0x3C021F80 @ 0x80109450): the WORLD is native via sceneNative exactly
// like the field (3D beats). The VOID beat (0x800BF9B4==5) has no 3D world/BG — the beat==5 guard inside
// sceneNative drops terrain/scene-table/backdrop and draws the vortex over black. Caption text = 2D producer.
void Render::renderSopNarration() {
  mCore->game->fps60.mTier1EligibleCur = true;
  DisplayPassGuard displayPass(mCore->mRender->mode);
  sceneNative();
}

// dialogTextNative — see render.h. Native producer for in-game dialog/prompt TEXT. Mirrors the guest
// glyph emitter FUN_8007CC00 (RE'd: docs/native-render-2d-panel.md Spec 3): reads the persistent glyph
// list @0x800ECB88 the dialog layout builds (FUN_8007C940; 8 B/entry, count=(s16)*0x1F80017E) and emits
// each glyph as a native op-0x65 font sprite (atlas tpage 0x1F). Read-only; emits nothing when count==0.
//
// Field widths per the real emitter's reads: x@0 = u16; y@2 = u8 (UNSIGNED — read as (ushort)); char@3
// (bit7 = double-width, low 7 bits select the CLUT); u@4 = u8; v@6 = u8. CLUT = ((char&0x7f)+0x1F0)<<6|0x3F.
//
// DEFERRED (labeled, not a bandaid): the emitter's highlight path (DialogBox+0x47==1 && +3==1 → forced
// CLUT 0x7CBE for the selected menu option) is not reproduced here — the flat-list read has no DialogBox
// pointer. Selected dialog options render in the normal per-glyph palette until the panel/box native owner
// (panelBuild family, Spec 1-3) lands and can pass the box highlight state. Non-highlight dialog is exact.
void Render::dialogTextNative() {
  Core* c = mCore;
  const int count = (int16_t)c->mem_r16(0x1F80017Eu);   // glyph count (FUN_8007C940 output)
  if (count <= 0 || count > 256) return;                 // no dialog / bogus
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  for (int i = 0; i < count; i++) {
    const uint32_t e = 0x800ECB88u + (uint32_t)i * 8u;   // 8-byte glyph entry
    const int gx = (int16_t)c->mem_r16(e + 0u);
    const int gy = c->mem_r8(e + 2u);                    // u8 (emitter reads (ushort)pbVar4[-1])
    const uint8_t ch = c->mem_r8(e + 3u);
    const int gu = c->mem_r8(e + 4u);
    const int gv = c->mem_r8(e + 6u);
    const int gw = (ch & 0x80) ? 16 : 8;                 // bit7 = double-width glyph
    const int gh = 16;
    const int clut = (((ch & 0x7F) + 0x1F0) << 6) | 0x3F;   // palette selected by glyph code (char&0x7f)
    int xs[4] = { gx + ox, gx + gw + ox, gx + ox, gx + gw + ox };
    int ys[4] = { gy + oy, gy + oy, gy + gh + oy, gy + gh + oy };
    int us[4] = { gu, gu + gw, gu, gu + gw };
    int vs[4] = { gv, gv, gv + gh, gv + gh };
    unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
    c->game->activeRq().push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                   /*tp_x=*/960, /*tp_y=*/256, /*mode=*/0, /*raw=*/1,
                                   (clut & 0x3F) * 16, (clut >> 6) & 0x1FF, 0, 0, 0, 0, 0, 0, 1023, 511);
  }
}

// FAIL-FAST for the one native renderer (USER 2026-07-15): no OT/GP0 fallback — a scene/layer lacking a
// native producer crashes with its identity, so the crash list is the rebuild backlog. See render.h.
void Render::abortUnimplemented(const char* scene) {
  Core* c = mCore;
  uint32_t stage   = c->mem_r32(0x801FE00Cu);
  uint32_t sm      = c->mem_r32(0x1F800138u);
  uint16_t sm4a    = c->mem_r16(0x801FE04Au);
  uint16_t sm4c    = c->mem_r16(0x801FE04Cu);
  uint32_t ovsig   = c->mem_r32(0x80109450u);   // loaded MODE overlay's first instruction (scene signature)
  uint16_t subm4c  = sm ? c->mem_r16(sm + 0x4Cu) : 0xFFFFu;
  fprintf(stderr,
    "\n[FATAL] unimplemented native rendering: %s\n"
    "        stage=0x%08X sm[0x4a]=%u sm[0x4c]=%u (task-sm[0x4c]=%u) overlay_sig=0x%08X\n"
    "        pc_render has no native producer for this scene/layer. Build it (native scene render) or\n"
    "        drive with PSXPORT_RENDER_PSX=1 (the reference renderer) to reach it. No OT-walk fallback.\n\n",
    scene, stage, sm4a, sm4c, subm4c, ovsig);
  fflush(stderr);
  abort();
}

// titleNative — see render.h. Read-only producer for the DEMO/title front-end (stage 0x801062E4 s2).
// Emits the title picture to the native render queue from source state (host-only), so pc_render renders
// it WITHOUT walking the guest OT. Increment 1: the black backdrop + the 2 logo sprites (exact geometry
// decoded from the guest op-0x65 packets — fixed title layout). Menu FT4 quads + font text land next as
// the menu builder (0x8007E2F8) is owned and the font glyph emitter gains a queue dual-emit.
void Render::titleNative() { Core* c = mCore;
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  // one textured 2D quad (screen rect x,y,w,h ; texel u,v of size w,h ; texpage/mode ; clut ; color).
  auto quad = [&](int layer, int x, int y, int w, int h, int u, int v, int tp_x, int tp_y, int mode,
                  int raw, int clut_x, int clut_y, int r, int g, int b) {
    int xs[4] = { x + ox, x + w + ox, x + ox,     x + w + ox };
    int ys[4] = { y + oy, y + oy,     y + h + oy, y + h + oy };
    int us[4] = { u, u + w, u, u + w };
    int vs[4] = { v, v, v + h, v + h };
    unsigned char rr[4] = { (unsigned char)r, (unsigned char)r, (unsigned char)r, (unsigned char)r };
    unsigned char gg[4] = { (unsigned char)g, (unsigned char)g, (unsigned char)g, (unsigned char)g };
    unsigned char bb[4] = { (unsigned char)b, (unsigned char)b, (unsigned char)b, (unsigned char)b };
    c->game->activeRq().push2dQuad(layer, /*order_2d_fg=*/1, xs, ys, us, vs, rr, gg, bb,
                                   tp_x, tp_y, mode, raw, clut_x, clut_y, 0, 0, 0, 0, 0, 0, 1023, 511);
  };
  // (0) black backdrop behind the art (RQ_BACKGROUND far band). Solid black, mode 3 = untextured.
  {
    int xs[4] = { 0, 320, 0, 320 }, ys[4] = { 0, 0, 240, 240 }, z[4] = { 0, 0, 0, 0 };
    unsigned char k[4] = { 0, 0, 0, 0 };
    c->game->activeRq().push2dQuad(RQ_BACKGROUND, /*order_2d_fg=*/0, xs, ys, z, z, k, k, k,
                                   0, 0, /*mode=*/3, /*raw=*/0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 511);
  }
  // (1) the title art = 2 op-0x65 raw textured sprites, fixed layout (decoded packet constants). Includes
  //     the baked TOMBA!2 logo + character art + the "(C) 1997-2000 WHOOPEE CAMP" copyright line.
  quad(RQ_BACKGROUND, 0,   -8, 256, 240, 0, 0, 640, 256, /*mode=*/1, /*raw=*/1, 640, 511, 0x80,0x80,0x80);  // tpage 0x9A
  quad(RQ_BACKGROUND, 256, -8,  64, 240, 0, 0, 768, 256, /*mode=*/1, /*raw=*/1, 640, 511, 0x80,0x80,0x80);  // tpage 0x9C
  // (2) the New Game / Load Game menu = 2 op-0x2C/0x2D FT4 text-image quads + a cursor icon (fn 0x8007E2F8
  //     family, docs/native-render-2d-panel.md). A HORIZONTAL 2-item menu (both rows at y=172). The SELECTED
  //     item is op 0x2D (raw, full-bright, color 0x80); the unselected is op 0x2C modulated by color 0x50
  //     (dimmed) — that IS the selection highlight. The cursor icon sits left of the selected item.
  //  LIVE selection (read-only, no guest write): sel = u8 at sm+0x68, sm = *(u32*)0x1F800138. Derived
  //  EMPIRICALLY from the reference renderer (PSXPORT_RENDER_PSX + otattr; Right/Left move the cursor):
  //    sel==0 -> New Game raw @(50,172), Load Game dim @(186,172), cursor @(32,168)
  //    sel==1 -> New Game dim,           Load Game raw,            cursor @(172,168)
  //  Cursor X = 32 + 140*sel (2-point fit for the 2-item menu; the xSel caller in the DEMO overlay
  //  0x80106xxx is not traced). Per-item rect/uv/clut are the item's own baked glyph strip — invariant to
  //  selection; only raw-vs-modulated (and the cursor row) flip. sel==0 byte-matches the old boot snapshot.
  uint32_t sm  = c->mem_r32(0x1F800138u);
  int      sel = sm ? c->mem_r8(sm + 0x68u) : 0;
  // one menu item: selected -> op 0x2D raw (bright, RGB ignored); unselected -> op 0x2C modulated by 0x50.
  auto menuItem = [&](int item, int x, int y, int w, int h, int u, int clut_y) {
    bool seld = (sel == item);
    int  raw  = seld ? 1 : 0;
    int  col  = seld ? 0x80 : 0x50;
    quad(RQ_OVERLAY, x, y, w, h, u, 1, 832, 256, /*mode=*/0, raw, 880, clut_y, col, col, col);
  };
  menuItem(0, 50,  172, 80, 16, 0,  509);   // "New Game"
  menuItem(1, 186, 172, 88, 16, 80, 510);   // "Load Game"
  int cursor_x = 32 + 140 * sel;            // sel 0 -> 32, sel 1 -> 172 (empirical 2-point fit)
  quad(RQ_OVERLAY, cursor_x, 168, 16, 16, 80, 112, 384, 0, /*mode=*/0, /*raw=*/1, 480, 247, 0x80,0x80,0x80); // cursor icon
}

void Render::sceneNative() { Core* c = mCore;
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  uint32_t saved = c->r[4];
  c->mRender->stats.snObjs = c->mRender->stats.snCmds = 0;
  // AREA-SCOPED CACHE trust latches (see render.h mSceneTableTrusted/mBackdropTrusted) — shared edge
  // detector, computed once per frame. Both SCENE_ENT_TABLE (0x800F2418) and PARALLAX_BG_SM
  // (0x800ED018, the backdrop tilemap struct below) go stale for a few ticks right when SOP narration
  // hands off to the field-area load; each latches back to trusted once ITS OWN structure shows the
  // natural re-zero its setup step performs before repopulating (see render.h for the full writeup).
  //
  // The handoff edge itself is read from the GAME submode dispatcher's own ownership switch
  // (sm[0x4a]==0: SOP field-mode machine owns the per-tick prepass/scroller; sm[0x4a]==1: the field-
  // area machine does, once it takes over) rather than the SOP overlay signature (0x80109450): the
  // area-load that repurposes PARALLAX_BG_SM's referenced memory (an in-progress CD stream landing
  // AS the same-tick RESET that also increments sm[0x4c]/sm[0x4a] — Sop::fieldMode case 4) clobbers it
  // 1-2 ticks BEFORE the overlay's own first-instruction word is overwritten by the incoming stream
  // (verified: sm[0x4c] leaves 0 the exact tick the garbage first appears, one tick ahead of sm[0x4a]
  // and two ahead of the overlay signature). sm[0x4c]!=0 while sm[0x4a]==0 is SOP's own one-shot
  // "narration ending, RESET case has fired" tail (it only ever increments once, at the single RESET
  // that ends the whole cutscene — not per-beat), so `sm4a==0 && sm4c==0` is "SOP genuinely still
  // owns this tick" without false-negatives across the narration's own beats.
  // The trust-latch EDGE DETECTOR is a once-per-frame state machine (mAreaCacheWasNarration is toggled by
  // the narration→field handoff edge). sceneNative runs exactly once per real logic frame (fps60 no longer
  // re-runs it for the in-between — docs/fps60-rework.md), so this always mutates the REAL frame's latches.
  {
    uint16_t sm4a = c->mem_r16(0x801fe04au), sm4c = c->mem_r16(0x801fe04cu);
    bool sop_narration_now = (sm4a == 0) && (sm4c == 0);
    if (sop_narration_now) {
      mSceneTableTrusted = true; mBackdropTrusted = true;      // narration's own prepass/scroller ticks both every tick
      mAreaCacheWasNarration = true;
    } else {
      if (mAreaCacheWasNarration) { mSceneTableTrusted = false; mBackdropTrusted = false; mAreaCacheWasNarration = false; }  // handoff edge
      if (!mSceneTableTrusted && c->mem_r8(0x800F2418u + 6u) == 0) mSceneTableTrusted = true;      // SCENE_ENT_TABLE owner reset seen
      if (!mBackdropTrusted   && c->mem_r8(0x800ed018u + 0x10u) == 0) mBackdropTrusted = true;     // PARALLAX_BG_SM owner reset seen (W==0)
    }
  }
  // (0) BACKDROP (sky + distant parallax hills) — the field's background drawer, dispatched natively. The
  // PSX path is 0x8003df04's 16-state jump table @0x80014fc0 keyed on mem_r8(0x800bf870), gated by
  // mem_r8(0x800bf873)==0. Only state 0 (→ tilemap drawer 0x80115598) is owned natively so far; other
  // fields' drawers are still-PSX and stay black here until ported (frontier). RQ_BACKGROUND → behind world.
  // Gated on mBackdropTrusted (see above): while untrusted, PARALLAX_BG_SM's tilemap pointer is the
  // ended narration's leftover, now aliasing the just-loaded field overlay's raw bytes — drawing it
  // produced a tiled noise/atlas-grid garbage frame (the narration-end -> fisherman-cutscene loading-
  // screen bug) where the oracle (pure PSX render) shows the expected black load-hold instead.
  // SOP VOID BEAT (narration *(u8*)0x800bf9b4 == 5): no 3D world, no BG — a pure vortex-over-black beat.
  // The backdrop struct (0x800ED018) and scene-table (0x800F2418) still hold the PRIOR beat's field data,
  // so terrain/scene-table/backdrop would paint a stale field/sea behind the swirl (later-281). Skip those
  // three passes for the void and draw an explicit black background; only the object pass runs (the vortex
  // node 0x800FBA68). beat!=5 everywhere else, so these guards are no-ops for the walkable field.
  const bool voidBeat = (c->mem_r8(0x800bf9b4u) == 5);
  if (voidBeat) {
    int xs[4] = { 0, 320, 0, 320 }, ys[4] = { 0, 0, 240, 240 }, z[4] = { 0, 0, 0, 0 };
    unsigned char k[4] = { 0, 0, 0, 0 };
    c->game->activeRq().push2dQuad(RQ_BACKGROUND, /*order_2d_fg=*/0, xs, ys, z, z, k, k, k,
                                   0, 0, /*mode=*/3, /*raw=*/0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 511);
  }
  if (!voidBeat && mBackdropTrusted && c->mem_r8(0x800bf873u) == 0) {
    uint32_t bgstate = c->mem_r8(0x800bf870u);
    if (bgstate == 0) backdropRender(0x800ed018u);
  }
  if (cfg_dbg("bgonly")) { c->r[4] = saved; return; }  // PROBE: backdrop only (test if ov_render_frame already drew the world)
  // AREA-INIT SUPPRESSION: on the GAME field-area-machine OBJECT-PLACEMENT init frame (stage GAME, sm[0x48]==2
  // RUNNING, sm[0x4a]==1 field-area-machine, sm[0x4e]==0 = the ov_field_run case-0 init), the new area's
  // objects have just been spawned but their MODELS are NOT yet attached — attach runs from per-object
  // behaviours on the following running frames (sm[0x4e]>=1), so each object's render-command geomblk
  // (cmd+0x40) still holds an unrelocated area-data pointer (0x8018Axxx). The real game does not draw the
  // field during this init (the area transition holds the screen faded black); drawing it here feeds garbage
  // prim counts into Render::gt3gt4 and overflows the render queue (later-275). Skip the field scene for that
  // frame; the backdrop above still draws. Read from task0's GAME state machine (persistent guest RAM, so it
  // is robust to the GAME loop's coroutine scheduling — a per-frame "did the field render run" latch is not,
  // because the field update and this display pass need not fall in the same native_step_frame).
  bool field_area_init = c->mem_r32(0x801fe00cu) == 0x8010637Cu   // GAME stage resident
                      && c->mem_r16(0x801fe048u) == 2             // sm[0x48] == RUNNING
                      && c->mem_r16(0x801fe04au) == 1             // sm[0x4a] == field area machine
                      && c->mem_r16(0x801fe04eu) == 0;            // sm[0x4e] == object-placement init (pre-attach)
  if (!field_area_init) {
    // (a) The render WALK CLUSTER (0x8003bf00/eec0/b588/bb50/bcf4/c048) is NO LONGER run from here
    // (USER 2026-07-07, issue #32): the substrate orchestrator executes it underneath every frame
    // (Render::frame — both render modes), so all its guest writes (walk-queue swaps, node bookkeeping,
    // per-node renderer dispatches, packet emission) happen on the faithful task's own call path,
    // byte-identical to the recomp reference. Re-running native lifts of those walks HERE was the
    // f26 divergence class: same writes from a foreign (display-phase) call context. The native lifts
    // (renderWalk/renderWalkSnapshot/rwalkAux*/rwalkB588/perObjRender/bgRender) are retired; this
    // display pass is READ-ONLY — it may not write guest memory or dispatch guest code. Per-object
    // depth tags for the guest-emitted billboard prims are lost until a read-only observer (e.g. an
    // EngineOverrides wrap teeing span info) is built — a KNOWN deferred render regression.
    // (a) TERRAIN — the field's render-list node whose render fn (node+24) is 0x8002AB5C, drawn by the
    // READ-ONLY float pass (real per-pixel depth). Pure reads: the node scan is the same enumeration the
    // substrate walk performs; the draw computes its matrices in host memory (native_terrain.cpp).
    // Enumeration+call moved to Render::terrainRenderAll() (submit.cpp) so Fps60's Tier-1 present-time
    // camera-lerp re-render (fps60.cpp) runs the identical sequence, not a hand-duplicated copy.
    if (!voidBeat) terrainRenderAll();
    // (b) SCENE TABLE (grass / props / sky-sea backdrop) — native world-coord render of 0x800F2418.
    // Gated on mSceneTableTrusted (computed once, top of this function — see render.h for the writeup).
    // Also skipped on the SOP void beat (stale field data — see the voidBeat guard above).
    if (mSceneTableTrusted && !voidBeat) fieldEntityRender(0x800F2418u);
    // (c)+(d) the field's OBJECTS + Tomba — factored into fieldObjectsRender() so Fps60's interp present
    // can re-run the SAME walk under lerped per-object transforms (docs/fps60-rework.md step 2b).
    fieldObjectsRender();
  }
  c->r[4] = saved;
  if (cfg_dbg("scenenative")) { int gpu_seen3d_this_frame(Core*); static int f = 0; if ((f++ % 60) == 0)
    fprintf(stderr, "[scenenative] objs=%ld cmds=%ld seen3d=%d\n", c->mRender->stats.snObjs, c->mRender->stats.snCmds, gpu_seen3d_this_frame(c)); }
}

// The field OBJECT pass — the (c)+(d) walk factored out of sceneNative (above) so it can be re-run at the
// fps60 interp present under lerped per-object transforms (mObjOverrideOn). Pure reads (entity lists,
// object nodes) + queue emits via perObjFlush → projComposeObject (→ Fps60::projObj) → gt3gt4. No trust-
// latch / per-frame-state mutation (those stay in sceneNative), so re-running it is safe under the
// present-time invariant. (d) Tomba: the player is not in the 3 entity lists (per-area callback tick);
// flushed the same generic way — this was the "Tomba invisible" fix.
void Render::fieldObjectsRender() {
  Core* c = mCore;
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  uint32_t saved = c->r[4];
  for (int h = 0; h < 3; h++) {
    uint32_t n = c->mem_r32(HEADS[h]);
    for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
      if (c->mem_r8(n + 1) == 0) continue;                             // per-frame visibility marker
      if (c->mem_r8(n + 8) == 0 || c->mem_r8(n + 9) == 0) continue;    // no render commands
      c->mRender->stats.snObjs++; c->mRender->stats.snCmds += c->mem_r8(n + 8);
      c->r[4] = n;
      perObjFlush();
    }
  }
  {
    uint32_t g = ActorTomba::G_ADDR;
    if (c->mem_r8(g + 8) != 0 && c->mem_r8(g + 9) != 0) {
      c->mRender->stats.snObjs++; c->mRender->stats.snCmds += c->mem_r8(g + 8);
      c->r[4] = g;
      perObjFlush();
    }
  }
  c->r[4] = saved;
}
// ===================================================================================================
// RETIRED 2026-07-07 (issue #32, USER: "PSX render path always active underneath; PC renderer
// shouldn't write to guest memory"): the native lifts of the walk cluster — perObjRender, bgRender,
// renderWalk (gen_func_8003C048), renderWalkSnapshot (8003BB50), rwalkAuxBcf4/Bf00/Eec0
// (8003BCF4/BF00/EEC0) and their per-type case tables. They re-ran the substrate walks' guest writes
// (queue swaps, node bookkeeping, guest renderer dispatches) from the display phase — a foreign call
// context whose guest-stack spills diverged from the recomp reference (SBS f26). The substrate
// orchestrator now executes the real walks underneath (Render::frame); this display pass is read-only.
// The RE'd case tables live in git history (this file @ commit 7989159) and docs/findings/render.md.
