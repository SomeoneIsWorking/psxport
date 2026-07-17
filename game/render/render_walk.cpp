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
#include "game_ctx.h"
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
      EObjXform w; rend(c)->projComposeObject(cmd, &w);
      rend(c)->projSetActive(&w);
      // OT base: node[0xd]&0xf == 4 selects a per-command sub-bucket (cmd[0x3f]*4 offset), else the base.
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4)
        otbase = otbase_ptr + ((c->mem_r8s(cmd + 0x3F)) << 2);
      // fps60 TRUE per-object tier: the object's world transform was captured (keyed by cmd) inside
      // projComposeObject above; the GT3/GT4 submit projects it. No per-prim key needed anymore.
      rend(c)->gt3gt4(geomblk, otbase);              // fully-native generic GT3/GT4 submit (no PSX fallback)
      rend(c)->projClearActive();
    }
    i++;
    if (i >= (int)c->mem_r8(node + 9)) break;
  }
}

// perObjFlushPreComposed — render.h banner. Same walk shape as perObjFlush; the transform comes from
// FACTORING the cmd's pre-composed MATRIX against the scene camera (wq_read_matrix/wq_factor_world,
// render_internal.h) and re-composing through projComposeObjectHost — camera applied exactly once,
// through the sceneCam choke (fps60-lerped at the interp re-run).
void Render::perObjFlushPreComposed() {
  Core* c = mCore;
  uint32_t node = c->r[4];
  if (c->mem_r8(node + 8) == 0) return;
  if (c->mem_r8(node + 9) == 0) return;
  uint32_t otbase_ptr = c->mem_r32(OTBASE_PTR);
  int i = 0;
  while (i < (int)c->mem_r8(node + 8)) {
    uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
    uint32_t geomblk = c->mem_r32(cmd + 0x40);
    if (geomblk != 0) {
      float crF[3][3], tr[3], objR[3][3], objT[3];
      wq_read_matrix(c, cmd + 0x18u, crF, tr);
      wq_factor_world(c, crF, tr, objR, objT);
      // projComposeCore expects Robj in raw int16 scale (4096 = 1.0); the factored objR is unit-scale.
      float Rraw[3][3];
      for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) Rraw[r][cc] = objR[r][cc] * 4096.0f;
      EObjXform w; rend(c)->projComposeObjectHost(Rraw, objT, &w);
      rend(c)->projSetActive(&w);
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4)
        otbase = otbase_ptr + ((c->mem_r8s(cmd + 0x3F)) << 2);
      rend(c)->gt3gt4(geomblk, otbase);
      rend(c)->projClearActive();
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
  // TILE V-OFFSET — the FIELD backdrop drawer (FUN_80115598) samples each tile at v = (tile&0xF0)+8, but
  // the SOP NARRATION backdrop drawer (FUN_8010C26C, the seaside/cutscene beats) samples at v = (tile&0xF0)
  // with NO +8 (RE'd from ram_sea.bin, issue #60). Applying the field's +8 to the SOP backdrop shifts the
  // atlas sample half a tile -> tile seams (verified: sea beat RMSE 40.2 -> 18.5 with vAdd=0). Pick the
  // variant from the resident overlay (data-derived, not a magic constant): the SOP narration overlay's
  // first-instruction signature @0x80109450 == 0x3C021F80. backdropRender is never called on the title, and
  // the field overlay evicts SOP, so this reads correctly in both the field and narration contexts, and at
  // both call sites (sceneNative real frame + Fps60 tier-1 interp re-render).
  const int vAdd = (c->mem_r32(0x80109450u) == 0x3C021F80u) ? 0 : 8;
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
  c->rsub.diag.beginObject(kBackdropDbgNode);
  for (int t8 = scrollY - 120;;) {
    int Y = (int16_t)((t8 & 0xFFF0) + yoff);
    int t6 = (int16_t)t2;                          // row byte offset (sign-extended)
    int t0 = coloff0;
    for (int t1 = scrollX - cx;;) {
      int X = (int16_t)((t1 & 0xFFF0) + xoff);
      uint16_t tile = c->mem_r16(map + (uint32_t)(t6 + t0));
      int u = (tile & 0xF) << 4, v = (tile & 0xF0) + vAdd;   // field +8 / SOP narration +0 (see top of fn)
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
      c->rsub.stats.snCmds++;
      t0 += 2; if (t0 >= rowstride) t0 = 0;        // column wrap
      t1 += 16;
      if (!((int16_t)t1 < t5)) break;
    }
    t2 += rowstride; if ((int16_t)t2 >= mapbytes) t2 -= mapbytes;  // row wrap
    t8 += 16;
    if (!((int16_t)t8 < outer_bound)) break;
  }
  c->rsub.diag.endObject();
}

// ---- pc_render scene DISPATCH (see render.h) --------------------------------------------------------
// Classify the current scene from the resident stage (0x801FE00C) + its sub-state selectors.
Render::SceneKind Render::classifyScene() {
  Core* c = mCore;
  // TASK-SWITCH HANDOFF GUARD (RE'd from the cooperative scheduler FUN_80051e60): a task slot's state
  // field @+0x00 is 0=empty, 2=ready, 3=re-registered/needs-fresh-context, 4=running. When task0 is in
  // state 3, its entry (+0x0c) was just reassigned (e.g. START.BIN -> DEMO front-end) but the new entry's
  // code has NOT run yet — so its substate fields (sm[0x48], …) still hold the PREVIOUS occupant's stale
  // values. Classifying by (entry, substate) here would misread that stale substate as a real scene (this
  // is why the START->DEMO handoff frame, entry=DEMO with sm[0x48]=3 left over from START, looked like a
  // bogus "DEMO substate 3"). During the 1-frame handoff the reference draws the black loader; do the same.
  constexpr uint32_t TASK0_STATE = 0x801FE000u;   // task0 slot, state @+0x00 (u16)
  constexpr uint16_t TASK_REINIT = 3;             // scheduler: entry (re)assigned, code not yet run
  if (c->mem_r16(TASK0_STATE) == TASK_REINIT) return SceneKind::Loading;
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
    case SceneKind::Loading:      renderLoading();      break;
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

// #0 TASK-SWITCH HANDOFF (task0 in scheduler state 3): the front-end task was just re-registered and its
// code hasn't run yet — nothing valid to draw for one frame. The reference shows the black loader here.
void Render::renderLoading() {
  mCore->game->fps60.mTier1EligibleCur = false;
  mCore->game->gpu.gpu_blank_display();
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
  const uint16_t s48 = c->mem_r16(0x801FE048u);
  if (s48 == 2 || s48 == 3) {
    DisplayPassGuard displayPass(c->rsub.mode);   // read-only: reads source state, emits host-only
    if (s48 == 2) titleNative();     // page 0 — New/Load title menu
    else          s3MenuNative();    // page 1 — the post-New-Game menu (Demo::s3)
    return;
  }
  // Black LOADING/TEARDOWN substates (verified black on the reference — NOT "missing rendering"):
  //  · s48 < 2 : the OP.FMV/SCEA boot ramp (movie skipped, accepted deferral — docs/tomba2-fmv-skip.md).
  //  · s48 == 5: `demo_frame_s5` LEAVE-DEMO — a ~2-frame task teardown (jal 0x80052078(2)) that kills the
  //    demo task and kicks the GAME load; the OT is empty, the screen holds black until GAME s48=2 (field).
  if (s48 < 2 || s48 == 5) { c->game->gpu.gpu_blank_display(); return; }
  // Any other front-end substate (load-game browser s4, page s6, attract s7) is REAL content with NO native
  // producer yet. Per USER (2026-07-15, restated): missing rendering CRASHES — no silent black-fill. The
  // crash names the substate so it becomes the next rebuild item.
  abortUnimplemented("DEMO/title front-end substate (sm[0x48] in {4,6,7,...}) — no native producer");
}

// #3 WALKABLE FIELD — native WORLD: terrain + entity/scene tables + objects + backdrop, real per-pixel
// depth. The 2D layer (HUD/text/dialog/billboards) comes from its own native producers, not the OT.
void Render::renderField() {
  mCore->game->fps60.mTier1EligibleCur = true;   // native field render runs -> fps60 tier-1 may re-render it
  DisplayPassGuard displayPass(mCore->rsub.mode);   // read-only invariant: aborts on any guest write
  sceneNative();
  // dialog/prompt text arrives via the FUN_8007CC00 tap (Panel::pushDialogGlyphs) at emit time
  cineBarsRender();     // cinematic letterbox bars (emits nothing when no cutscene bars are active)
}

// #4 HUT/DOOR INTERIOR (task-sm[0x4c]==3): OBJECTS-ONLY. The room is entity-list object 0x800FD850
// (HEADS[1]); NPCs/props are HEADS[0..1]; Tomba is the G block — fieldObjectsRender walks them all via
// perObjFlush -> gt3gt4 with real depth + the live interior camera. Skips the exterior terrain/scene-table/
// backdrop (village data) — the substrate's reduced frameX pass. 2D bubble = native producer when rebuilt.
void Render::renderHutInterior() {
  // BREAK-FIRST (USER 2026-07-16): the authored hut interior (GAME sm[0x4c]==3) has NO native WORLD
  // producer — only objects (fieldObjectsRender) were drawn, the room itself was never built natively,
  // and the scene is not tier1-eligible, so fps60 could not interpolate it (it re-presented the captured
  // queue verbatim → 30fps). Per the standing rule this file already applies to renderTitle's unported
  // substates: an unported render path does NOT fall back to a partial/30fps draw — it crashes with its
  // identity so the interior becomes the next native rebuild item (a real per-object interior WORLD
  // producer that is tier1-eligible → 60fps like the field, at which point restore the object render).
  mCore->game->fps60.mTier1EligibleCur = false;
  abortUnimplemented("hut interior (GAME sm[0x4c]==3) — no native world producer (objects-only 30fps render removed)");
}

// #5 SOP INTRO NARRATION (overlay-sig 0x3C021F80 @ 0x80109450): the WORLD is native via sceneNative exactly
// like the field (3D beats). The VOID beat (0x800BF9B4==5) has no 3D world/BG — the beat==5 guard inside
// sceneNative drops terrain/scene-table/backdrop and draws the vortex over black. Caption text = 2D producer.
void Render::renderSopNarration() {
  mCore->game->fps60.mTier1EligibleCur = true;
  DisplayPassGuard displayPass(mCore->rsub.mode);
  sceneNative();
  cineBarsRender();     // cinematic letterbox bars (the SOP narration is a cutscene)
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
  uint16_t sm48    = c->mem_r16(0x801FE048u);    // DEMO/front-end substate selector
  uint16_t subm4c  = sm ? c->mem_r16(sm + 0x4Cu) : 0xFFFFu;
  fprintf(stderr,
    "\n[FATAL] unimplemented native rendering: %s\n"
    "        stage=0x%08X sm[0x48]=%u sm[0x4a]=%u sm[0x4c]=%u (task-sm[0x4c]=%u) overlay_sig=0x%08X\n"
    "        pc_render has no native producer for this scene/layer. Build it (native scene render) or\n"
    "        drive with PSXPORT_RENDER_PSX=1 (the reference renderer) to reach it. No OT-walk fallback.\n\n",
    scene, stage, sm48, sm4a, sm4c, subm4c, ovsig);
  fflush(stderr);
  abort();
}

// emitMenuFt4 — see render.h. Reproduces FUN_8007e1b8's POLY_FT4 path: resolve the template, decode each
// 16-byte entry, emit a native FT4 quad from the game's OWN geometry (guest RAM). Decoder validated
// field-for-field against the title's known-good quads (docs/findings/render.md '#2b').
void Render::emitMenuFt4(int anchorX, int anchorY, uint32_t templateIdx, uint32_t attr, int layer) {
  Core* c = mCore;
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  const uint32_t tptr = c->mem_r32(0x80017334u + templateIdx * 4u);   // (&PTR_DAT_80017334)[idx]
  const int      hdr  = (int16_t)c->mem_r16(tptr);                    // *param_2 -> header index
  const uint32_t psv  = 0x80158000u + (uint32_t)hdr * 4u;             // base + hdr*4
  const int      cnt  = (int16_t)c->mem_r16(psv);                     // entry count
  const uint32_t data = 0x80158000u + c->mem_r16(psv + 2u);           // base + data offset
  const int raw = ((attr & 0xF0u) == 0) ? 1 : 0;                      // high nibble 0 -> RAW, else modulated
  const unsigned char col = raw ? 0x80 : (unsigned char)attr;
  for (int k = 0; k < cnt && k < 16; k++) {
    const uint32_t e = data + (uint32_t)k * 16u;                      // 16-byte FT4 template entry
    const int x0 = anchorX + (int8_t)c->mem_r8(e + 14u);             // v0 = anchor + entry[14,15]
    const int y0 = anchorY + (int8_t)c->mem_r8(e + 15u);
    const int w  = c->mem_r8(e + 10u), h = c->mem_r8(e + 11u);       // width @10, height @11
    const uint16_t clut = c->mem_r16(e + 2u), tpage = c->mem_r16(e + 6u);
    int xs[4] = { x0 + ox, x0 + w + ox, x0 + ox,     x0 + w + ox };
    int ys[4] = { y0 + oy, y0 + oy,     y0 + h + oy, y0 + h + oy };
    // per-vertex UVs straight from the entry (TL,TR,BL,BR = entry[0,1]/[4,5]/[8,9]/[12,13]).
    int us[4] = { c->mem_r8(e+0u), c->mem_r8(e+4u), c->mem_r8(e+8u),  c->mem_r8(e+12u) };
    int vs[4] = { c->mem_r8(e+1u), c->mem_r8(e+5u), c->mem_r8(e+9u),  c->mem_r8(e+13u) };
    unsigned char cc[4] = { col, col, col, col };
    const int tp_x = (tpage & 0xF) * 64, tp_y = ((tpage >> 4) & 1) * 256, mode = (tpage >> 7) & 3;
    const int clut_x = (clut & 0x3F) * 16, clut_y = (clut >> 6) & 0x1FF;
    c->game->activeRq().push2dQuad(layer, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                   tp_x, tp_y, mode, raw, clut_x, clut_y, 0, 0, 0, 0, 0, 0, 1023, 511);
  }
}

// menuChrome — see render.h. The black backdrop + the 2 logo sprites (FUN_80106690), shared by every
// front-end menu page. The logos are op-0x65 raw sprites (fixed layout, decoded packet constants).
void Render::menuChrome() { Core* c = mCore;
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  { int xs[4] = { 0, 320, 0, 320 }, ys[4] = { 0, 0, 240, 240 }, z[4] = { 0, 0, 0, 0 };
    unsigned char k[4] = { 0, 0, 0, 0 };
    c->game->activeRq().push2dQuad(RQ_BACKGROUND, 0, xs, ys, z, z, k, k, k,
                                   0, 0, /*mode=*/3, /*raw=*/0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 511); }
  auto logo = [&](int x, int w, int tp_x) {                          // tpage 0x9A(640)/0x9C(768), 8bpp
    int xs[4] = { x+ox, x+w+ox, x+ox, x+w+ox }, ys[4] = { -8+oy, -8+oy, 232+oy, 232+oy };
    int us[4] = { 0, w, 0, w }, vs[4] = { 0, 0, 240, 240 };
    unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
    c->game->activeRq().push2dQuad(RQ_BACKGROUND, 1, xs, ys, us, vs, cc, cc, cc,
                                   tp_x, 256, /*mode=*/1, /*raw=*/1, 640, 511, 0, 0, 0, 0, 0, 0, 1023, 511); };
  logo(0, 256, 640); logo(256, 64, 768);
}

// menuItemsAndCursor — see render.h. Reproduces FUN_80106824(param1, param2): the cursor (template 0x98
// at the game's cursor-X table @0x80107704) then the two item text-images (page-0 {0x8e,0x8f} title, or
// page-1 {0x90,0x91} s3), the param2-selected item RAW/bright and the other modulated 0x50/dim.
void Render::menuItemsAndCursor(int param1, int param2) { Core* c = mCore;
  static const uint32_t TMPL[2][2] = { { 0x8Eu, 0x8Fu }, { 0x90u, 0x91u } };
  const uint32_t t0 = TMPL[param1 & 1][0], t1 = TMPL[param1 & 1][1];
  const uint32_t a0 = (param2 == 0) ? 0u : 0x50u, a1 = (param2 == 0) ? 0x50u : 0u;
  const int cx = (int16_t)c->mem_r16(0x80107704u + (uint32_t)(param2 * 2 + param1 * 4));  // cursor anchor X
  // Draw items first, cursor last: where a wide item (page-1 item0 at x43) overlaps the cursor (x32..48),
  // the reference draws the cursor ON TOP (verified by pixel-diff: cursor-last -> RMSE ~0; cursor-first
  // left a 15px seam at the overlap). The guest OT links cursor after the items but is walked cursor-first.
  emitMenuFt4(90,  180, t0, a0, RQ_OVERLAY);      // item 0 anchor (90,180)
  emitMenuFt4(230, 180, t1, a1, RQ_OVERLAY);      // item 1 anchor (230,180)
  emitMenuFt4(cx, 0xB0, 0x98u, 0u, RQ_OVERLAY);   // cursor (template 0x98, raw), y anchor 176
}

// titleNative — see render.h. Read-only producer for the DEMO/title front-end page 0 (sm[0x48]==2, the
// New/Load menu). Chrome (backdrop + logos) + the page-0 menu, entirely data-driven off the guest menu
// templates (no hand-decoded constants). Selection sel = sm[0x68] (Demo::s2SubMachine writes it); the
// guest calls FUN_80106824(0, sm[0x68]!=0).
void Render::titleNative() { Core* c = mCore;
  menuChrome();
  uint32_t sm = c->mem_r32(0x1F800138u);
  int sel = sm ? c->mem_r8(sm + 0x68u) : 0;
  menuItemsAndCursor(0, (sel != 0) ? 1 : 0);
}

// s3MenuNative — see render.h. The page-1 menu (sm[0x48]==3, reached by confirming New Game). Same chrome,
// page-1 templates; the guest (Demo::s3 -> s3SubMachine) calls FUN_80106824(1, sm[0x68]!=2).
void Render::s3MenuNative() { Core* c = mCore;
  menuChrome();
  uint32_t sm = c->mem_r32(0x1F800138u);
  int s68 = sm ? c->mem_r8(sm + 0x68u) : 2;
  menuItemsAndCursor(1, (s68 != 2) ? 1 : 0);
}

// Per-frame WORLD-pass gates (render.h): one definition each, read by BOTH the real sceneNative pass and
// Fps60::tier1Render's interp re-render, so the two frames of a 60fps pair agree on which passes run.
// SOP VOID BEAT (narration *(u8*)0x800bf9b4 == 5): no 3D world, no BG — a pure vortex-over-black beat.
// The backdrop struct (0x800ED018) and scene-table (0x800F2418) still hold the PRIOR beat's field data,
// so terrain/scene-table/backdrop would paint a stale field/sea behind the swirl (later-281). beat!=5
// everywhere else, so this is a no-op for the walkable field.
bool Render::worldVoidBeat() const { return mCore->mem_r8(0x800bf9b4u) == 5; }
// AREA-INIT SUPPRESSION: on the GAME field-area-machine OBJECT-PLACEMENT init frame (sm[0x48]==2 RUNNING,
// sm[0x4a]==1 field-area-machine, sm[0x4e]==0 init), the new area's objects are spawned but their MODELS
// are NOT yet attached — each cmd's geomblk (cmd+0x40) still holds an unrelocated area-data pointer. The
// real game holds the screen faded black here; drawing feeds garbage prim counts into gt3gt4 and
// overflows the render queue (later-275). Read from task0's GAME state machine (persistent guest RAM).
bool Render::fieldAreaInit() const {
  Core* c = mCore;
  return c->mem_r32(0x801fe00cu) == 0x8010637Cu   // GAME stage resident
      && c->mem_r16(0x801fe048u) == 2             // sm[0x48] == RUNNING
      && c->mem_r16(0x801fe04au) == 1             // sm[0x4a] == field area machine
      && c->mem_r16(0x801fe04eu) == 0;            // sm[0x4e] == object-placement init (pre-attach)
}

void Render::sceneNative() { Core* c = mCore;
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  uint32_t saved = c->r[4];
  c->rsub.stats.snObjs = c->rsub.stats.snCmds = 0;
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
  // SOP VOID BEAT (worldVoidBeat above): skip terrain/scene-table/backdrop and draw an explicit black
  // background; only the object pass runs (the vortex node 0x800FBA68).
  const bool voidBeat = worldVoidBeat();
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
  // AREA-INIT SUPPRESSION (fieldAreaInit above): skip the field scene for the object-placement init frame
  // (models not yet attached — drawing overflows the queue, later-275); the backdrop above still draws.
  bool field_area_init = fieldAreaInit();
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
    // override-registry entry teeing span info) is built — a KNOWN deferred render regression.
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
    cfg_logf("scenenative", "objs=%ld cmds=%ld seen3d=%d", c->rsub.stats.snObjs, c->rsub.stats.snCmds, gpu_seen3d_this_frame(c)); }
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
      const uint32_t type = c->mem_r8(n + 0xB);
      // CUSTOM-RENDER-FN node (type 0x20): the node draws via a render fn at n+0x18 (substrate default-
      // case dispatch), NOT the cmd array. Known: the SOP narration SWIRL (0x8010BF54) — draw it natively
      // (narration_swirl.cpp). Guard on the SOP overlay being resident (sig @0x80109450): a stale node
      // whose fn points into an EVICTED overlay is normal (later-275 dangling-pointer case) — skip it.
      // Other type-0x20 fns stay skipped like before (tracked by #3b's completeness gate, not a crash).
      if (type == 0x20) {
        if (c->mem_r32(n + 0x18) == 0x8010BF54u && c->mem_r32(0x80109450u) == 0x3C021F80u) {
          c->rsub.stats.snObjs++;
          rend(c)->narrationSwirlRender(n);
        }
        continue;
      }
      if (c->mem_r8(n + 8) == 0 || c->mem_r8(n + 9) == 0) continue;    // no render commands
      // TYPE-CORRECT ROUTING (#67 cont.; tables RE'd from the LIVE walk jump tables — the substrate
      // routes each node TYPE to a class-specific renderer, and the cmd+0x18 field's MEANING differs
      // by class). Only the perObjRenderDispatch family stores an OBJECT rotation there (camera∘object
      // compose = perObjFlush). The F174 family (renderWalk table 0x80014DB8 types 1 and 4 — type 4 is
      // the text-label node, whose glyphs textLabelEmit already covers) stores a PRE-COMPOSED
      // camera∘object MATRIX — perObjFlush's compose applied the camera TWICE for those. Route them
      // through the factored-matrix flush instead. Every other type (billboard composers — covered by
      // billboardsRender; still-unowned overlay custom renderers) draws NOTHING here (USER 2026-07-16:
      // "don't render any unowned things"), rather than a guessed-transform mesh.
      //   HEADS[1] 0x800F2624 (renderWalk 0x80014DB8): mesh {0,15} · pre-composed {1,4}
      //   HEADS[2] 0x800F2738 (objListWalk4 0x80015000): mesh {0,1,15} (1 = EF30 mesh + B704 beams)
      //   HEADS[0] 0x800FB168: table not yet RE'd — keep the flush-all behavior (existing).
      bool mesh = true, pre = false;
      if (h == 1) { mesh = (type == 0 || type == 15); pre = (type == 1 || type == 4); }
      else if (h == 2) { mesh = (type == 0 || type == 1 || type == 15); }
      if (!mesh && !pre) continue;
      c->rsub.stats.snObjs++; c->rsub.stats.snCmds += c->mem_r8(n + 8);
      c->r[4] = n;
      if (pre) perObjFlushPreComposed();
      else     perObjFlush();
    }
  }
  {
    uint32_t g = ActorTomba::G_ADDR;
    if (c->mem_r8(g + 8) != 0 && c->mem_r8(g + 9) != 0) {
      c->rsub.stats.snObjs++; c->rsub.stats.snCmds += c->mem_r8(g + 8);
      c->r[4] = g;
      perObjFlush();
    }
  }
  // BILLBOARDS (#67): the display-pass particle-quad producer — projects the records billboardEmit
  // captured this logic frame through the float camera (fps60-lerped at the interp re-run). Lives in
  // this walk so field, hut interior and Fps60::tier1Render all derive it the same way.
  billboardsRender();
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
