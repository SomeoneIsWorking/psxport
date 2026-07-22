// game/render/fps60.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// RENDERER-INTERNAL, one-frame-behind interpolation (docs/fps60-rework.md; UNIFIED-PATH redesign
// 2026-07-15). PRINCIPLE (USER): there is no difference between how a real frame and an interpolated frame
// are drawn EXCEPT the lerp — all drawing flows through the same logic, and the lerp lives in the INPUTS.
// The interp present RE-RUNS the real render one frame behind, with every input served a lerp(prev,cur,t)
// through its capture/override choke: CAMERA (sceneCam / mCamCur/mCamPrev), PER-OBJECT TRANSFORM (projObj /
// mObjCur/mObjPrev, keyed by cmd), BACKDROP scroll (bgScroll / mBgCur/mBgPrev). tier1Render re-runs the
// field WORLD passes (terrainRenderAll + fieldEntityRender + fieldObjectsRender + backdropRender — the SAME
// calls the real sceneNative makes) under those lerped inputs into the isolated mSink; present_vk merges
// mSink with mRqCur's remaining prims by (layer,seq): 2D HUD/overlay (screen-space, verbatim, no lerp
// needed) AND every GUEST-EXECUTION-TIME drawable (RQ_WORLD but has_xyf==0 — no display-pass producer
// to re-run, so they draw verbatim on BOTH presents; they step at 30Hz until each emitter is ported
// into the display pass per the REDIRECT doctrine, but they can no longer flicker).
// The authored sub-scene (hut interior, sm[0x4c]==3) has NO native world producer and is not tier1-
// eligible, so it could never be interpolated — it used to present the captured queue verbatim (30fps).
// Per BREAK-FIRST (USER 2026-07-16) renderHutInterior now aborts-with-identity instead of that partial
// render; the interior is a rebuild-frontier item, not a 30fps fallback.
//
// TIER 1 (docs/fps60-rework.md "Object-tier attempt 2026-07-14", extended to fieldEntityRender): the
// QUEUE-LERP heuristic above does not own CAMERA-ONLY world-static geometry — it is replaced there by a
// REAL re-render. At the interp present, `tier1Render` re-runs `Render::terrainRenderAll()` AND
// `Render::fieldEntityRender()` (the SAME two call sequences the real per-logic-frame walk uses — both
// project through `projComposeCamera`/the terrain camera compose, camera-only, no per-object transform)
// under a LERPED camera (mCamCur/mCamPrev, captured every real frame from the same source both readers use
// — `sceneCam`), with their output redirected into an ISOLATED sink (`mSink`, a second RenderQueue) via
// `Game::rqRedirect` so the live queue the next real frame is about to build is never touched. INVARIANT:
// present_vk (hence this re-render) runs from `Engine::frameUpdate`, which native_boot.cpp calls BEFORE
// this iteration's `pcSched.step()` (game logic) and `drawOTag` (which builds the NEXT real queue) — so at
// present time no logic tick for "this" iteration has run yet, and re-running either pass re-reads the
// exact same guest state the real call already read this interval (record arrays: static per-area data,
// not per-frame mutable state). Host-computed matrices only (the lerped camera); no guest writes — same
// `DisplayPassGuard` discipline the real terrain call uses. World prims tier-1 owns (RQ_WORLD,
// dbg_node==kTerrainDbgNode or kSceneTableDbgNode — see render_queue.h) are skipped in the mRqCur
// merge (see isTier1Owned in fps60.cpp) so they are drawn exactly once.
//
// SCREEN-SPACE BACKDROP (the scrolling sky/parallax tilemap, Render::backdropRender — was the file-local
// render_bg_tilemap_native) is a LAYER-TRANSFORM tier, not a camera tier: the whole layer's only per-frame
// motion is its scroll offset (game-logic-driven — ParallaxBg::step, not camera projection), so it is
// interpolated as ONE transform, not per-prim: `tier1Render` re-runs `Render::backdropRender()` (the SAME
// native pass the real per-logic-frame call uses) with the scroll offset overridden to a WRAP-AWARE lerp
// of the two real frames' captured offsets (mBgCur/mBgPrev, `bgScroll()`), output redirected into `mSink`
// alongside terrain/scene-table (only backdropRender's OWN prims — kBackdropDbgNode; see isTier1Owned
// in fps60.cpp).
//
// Everything the tier-1 re-run does not own (screen-space HUD/2D, guest-time records) presents
// VERBATIM from the captured queue on both frame kinds; each such emitter graduates into the
// display-pass re-run as it is RE'd and ported native (docs/fps60-rework.md "REDIRECT").
//
// HOST-ONLY (the READ-ONLY OVERLAY invariant): every capture is a guest READ (at queue-flush time for the
// per-frame queue snapshot / camera, or at present time for tier1Render's re-read of unchanged state per
// the invariant above); every store is host memory (mSink, the camera slots). Per-Core shared render state
// touched incidentally by re-running terrainRenderAll (ProjParams' published camview + H/OFX/OFY) is
// snapshotted before and restored after, so nothing else observes the lerped camera. When g_mods.fps60 is
// off, none of this arms and the 30fps path is byte-identical.
//
// De-globalization (2026-06-19): all state lives on this Fps60 instance owned by Game (game.h), reached
// via core->game->fps60; every touching function is a method (callers use c->game->fps60.method(...)).
#ifndef GAME_RENDER_FPS60_H
#define GAME_RENDER_FPS60_H
#include <stdint.h>
#include <unordered_map>
#include <vector>
#include "render_queue.h"   // RqItem, RenderQueue (mSink — Tier-1's isolated capture sink)

struct Core;
class Game;

// logic-rate detector (validated lrate_proto): votes on the number of frames each projected-geometry
// fingerprint is HELD, so the tier knows how many in-betweens to synthesize (Tomba2 logic = 30fps → 1).
typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;

// ---- Fps60 — the 60fps tier's per-instance interpolation state + methods ------------------------------
struct Fps60 {
  Game* game = nullptr;   // owner back-pointer (set in Game()) — gates via game->mods.fps60
  // ---- logic-rate detector (kept) --------------------------------------------------------------------
  uint64_t mFrameHash = 1469598103934665603ull;   // per-frame projected-geometry fingerprint (rate input)
  long     mFrameGeom = 0;                          // #verts folded this frame (0 => idle frame)
  long     mFence     = 0;                          // logic-frame counter
  RateDet  mRd = { 0, 0, 2, {}, 0 };
  void fold(uint32_t v);                            // fold a projected SXY into the frame fingerprint
  void rtp(uint32_t op);                            // gte RTP tap (fps60 gate) → fold the new SXY(s)
  void frame_commit(Core* core);                    // per-logic-frame fence + present orchestration

  // ---- shared camera reader ----------------------------------------------------------------------------
  // Scene camera read choke: fills R(int16 units)/T/ofx/ofy/H from the scratchpad view matrix + the GTE
  // projection constants. This is the ONE reader the whole native projection path uses (projComposeCore /
  // projComposeCamera / native_terrain) — not fps60-specific; it lives here because it predates the split
  // and every caller already reaches it via c->game->fps60.sceneCam(...).
  void sceneCam(Core* c, float R[3][3], float T[3], float& ofx, float& ofy, float& H);

  // ---- TIER 1: camera-lerp native world (terrain) re-render (docs/fps60-rework.md) ---------------------
  // Two-slot camera store, captured by sceneCam() every REAL (non-override) call — the SAME source
  // terrainRender() reads, rotated cur->prev in lockstep with mRqCur/mRqPrev (present_vk's end-of-frame
  // swap). R is raw int16-unit rows (undivided, the sceneCam/native_terrain convention); T/ofx/ofy/H as
  // sceneCam returns them.
  struct Fps60Cam { float R[3][3] = {{0,0,0},{0,0,0},{0,0,0}}; float T[3] = {0,0,0};
                    float ofx = 0, ofy = 0, H = 0; };
  Fps60Cam mCamCur, mCamPrev;
  bool     mCamOverrideOn = false;   // set only while tier1Render() is re-invoking terrainRenderAll()
  Fps60Cam mCamOverride;             // the lerped camera sceneCam() returns while mCamOverrideOn
  RenderQueue* mSink = nullptr;       // ISOLATED capture sink (Game::rqRedirect points here during the
                                      // re-render) — never the live `game->rq` the next real frame builds.
                                      // Heap-allocated lazily (RQ_MAX items is ~16MB — same reason mRqCur/
                                      // mRqPrev/mRqLerp below are `new[]`, not embedded arrays).
  long mTier1PrimsThisFrame = 0;     // telemetry: WORLD (terrain+scene-table) prims tier1Render drew into mSink
  // #50: tier1Render re-renders the native FIELD passes (terrain/scene-table) on the interp frame. During an
  // authored OT sub-scene (hut interior, #49) or any beat where the real frame did NOT run sceneNative, there
  // is no native field to re-render — running it anyway draws the exterior field on interp frames only
  // (every-other-frame flicker to the exterior). Set true per real frame from the render dispatch
  // (game_tomba2.cpp) IFF the native field render ran this frame; tier1Render is skipped otherwise.
  bool mTier1EligibleCur = true;
  void tier1Render(Core* core, float t);   // re-run terrainRenderAll() under lerp(mCamPrev,mCamCur,t) into mSink

  // ---- TIER 1 BACKDROP: game-logic-scroll LAYER-TRANSFORM lerp (docs/fps60-rework.md) -----------------
  // Two-slot host capture of PARALLAX_BG_SM's per-frame-varying scroll offset (0x800ED018+0x28/+0x2A),
  // captured by bgScroll() on every REAL (non-override) Render::backdropRender() call — mirrors sceneCam's
  // self-capture. Everything else backdropRender reads (W/H/tilemap ptr/tpage/clutbase/wrap-moduli) is
  // static per-area config, unchanged while the layer runs, so it is safe to re-read directly at present
  // time (same invariant as terrain/scene-table's static geometry) without a capture slot.
  struct Fps60Bg { int scrollX = 0, scrollY = 0; };
  Fps60Bg mBgCur, mBgPrev;
  bool    mBgOverrideOn = false;   // set only while tier1Render() is re-invoking backdropRender()
  Fps60Bg mBgOverride;             // the wrap-lerped scroll offset backdropRender() reads while mBgOverrideOn
  // bgScroll: the scroll-offset read choke Render::backdropRender() calls instead of reading t4+0x28/+0x2A
  // directly — real call: reads + captures into mBgCur; present-time override: returns mBgOverride.
  void bgScroll(Core* c, uint32_t t4, int& scrollX, int& scrollY);

  // ---- PER-OBJECT TRANSFORM choke (UNIFIED-PATH redesign 2026-07-15, docs/fps60-rework.md) ------------
  // The object's world rotation/position (Robj cmd+0x18, Tobj cmd+0x2C), the last INPUT still read live
  // by the render (projComposeObject). Given the SAME capture/override shape as sceneCam so the interp
  // present can re-run the real object walk with lerped transforms. Real projComposeObject call:
  // read live + capture into mObjCur[cmd]. Interp present
  // (mObjOverrideOn): return lerp(mObjPrev[cmd], mObjCur[cmd], mT). `cmd` = the object's stable render-
  // command block (node+0xC0[i]) = its per-object identity across frames.
  struct Fps60Obj { float R[3][3]; float T[3]; };
  std::unordered_map<uint32_t, Fps60Obj> mObjCur, mObjPrev;
  bool mObjOverrideOn = false;   // set only while the interp present re-runs the object walk
  void projObj(Core* c, uint32_t cmd, float Robj[3][3], float Tobj[3]);

  // ---- present (interpolated in-between + real frame, paced 60fps 1-frame-behind) --------------------
  RqItem* mRqCur  = nullptr;    // this logic frame's resolved queue snapshot (captured at flush)
  RqItem* mRqPrev = nullptr;    // previous frame's snapshot (Q[N-1])
  int mNCur = 0, mNPrev = 0, mHavePrev = 0;
  void rq_capture(const RqItem* items, int n);      // copy the sorted queue snapshot
  void present_vk(Core* core);                      // build+present the in-between, then the real frame
  // presentPass — THE ONE PLACE A FRAME IS BUILT AND EMITTED. Both presents call it; `t` is the ONLY
  // difference between them (USER 2026-07-22: "there should be just one site, the only difference should
  // be whether to lerp"). t=1 serves every lerped input its CURRENT value, so the real frame is just the
  // degenerate in-between — it is not a second code path that happens to agree. Proven, not asserted:
  // with PSXPORT_FPS60_TFORCE=1 the two presents are pixel-identical, 0/76800 over 10 consecutive frames.
  void presentPass(Core* core, float t);
  void presentRotate();                             // rotate the cur/prev capture slots after both presents
  int  mDbg = -1;                                    // PSXPORT_DEBUG=fps60 lazy latch

  // ---- interp parameter + telemetry (UNIFIED PATH, 2026-07-15) ---------------------------------------
  // The interp present is the SAME render re-run under lerped inputs (camera + per-object transforms +
  // backdrop scroll) into mSink, merged with mRqCur's 2D verbatim — no prim matching. mT is the in-between
  // parameter both the camera lerp (tier1Render) and projObj's per-object lerp share.
  float mT = 0.5f;                    // in-between parameter (t=0.5 for one midpoint at 30->60fps)
  // BACKDROP telemetry: prims tier1Render drew into mSink for the backdrop layer (RQ_BACKGROUND) this
  // present, counted separately from mTier1PrimsThisFrame (terrain+scene-table+objects, RQ_WORLD).
  long mBackdropPrimsThisFrame = 0;

  // ---- per-present frame dump (debug channel `fps60dump`, REPL `debug fps60dump`) ---------------------
  // Writes one PNG per PRESENTED frame (real AND interp) to scratch/framedump/, so a Python script can
  // walk the sequence and check whether interpolated frames sit between their neighboring real frames
  // (fps60 correctness) instead of teleporting. Reuses the same VRAM-readback writer as REPL `shot`
  // (gpu_vk_shot / gpu_native_shot) — no new pixel-readback path. Capped at kDumpMax files so an
  // unbounded run can't fill disk; toggling the channel back on resets the cap.
  static constexpr int kDumpMax = 600;
  int mDumpSeq = 0;
  void dumpPresent(Core* core, bool interp);          // called from present_vk after each present pass
  ~Fps60();
};

#endif // GAME_RENDER_FPS60_H
