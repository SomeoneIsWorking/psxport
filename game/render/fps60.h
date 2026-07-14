// game/render/fps60.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// RENDERER-INTERNAL, one-frame-behind interpolation (docs/fps60-rework.md, redesign 2026-07-14). The
// renderer holds the last TWO real frames' resolved render-queue prims (double-buffered by rq_capture) and
// presents one frame behind: slot A draws lerp(Q[N-1], Q[N], t=0.5) via matchAndLerp's provenance-matched
// vertex lerp, slot B draws Q[N] verbatim. ONE draw path — both slots drain an RqItem list through the same
// `RenderQueue::emitItem` the 30fps path uses.
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
// `DisplayPassGuard` discipline the real terrain call uses. World prims tier-1 now owns (RQ_WORLD,
// dbg_node==kTerrainDbgNode or kSceneTableDbgNode — see render_queue.h) are excluded from matchAndLerp's
// queue-lerp entirely (see kTier1Sink in fps60.cpp) so they are drawn exactly once.
//
// SCREEN-SPACE BACKDROP (the scrolling sky/parallax tilemap, render_walk.cpp render_bg_tilemap_native) is
// the OPPOSITE case: its motion is game-logic scroll, not camera projection, so it must NEVER be lerped
// OR re-rendered — it is excluded from the queue-lerp and drawn VERBATIM from Q[N-1] every interp present
// (fps60.cpp kBackdropVerbatim).
//
// The match heuristic (matchAndLerp) remains for everything tier-1/2 don't yet own (objects, HUD, 2D):
// docs/fps60-rework.md's "REDIRECT" flags it as hack debt to be replaced, emitter by emitter, by real
// per-element identity as each quad-emitting guest fn is RE'd and ported native — it is NOT deleted yet.
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
  long mTier1PrimsThisFrame = 0;     // telemetry: prims tier1Render drew into mSink this present
  void tier1Render(Core* core, float t);   // re-run terrainRenderAll() under lerp(mCamPrev,mCamCur,t) into mSink

  // ---- present (interpolated in-between + real frame, paced 60fps 1-frame-behind) --------------------
  RqItem* mRqCur  = nullptr;    // this logic frame's resolved queue snapshot (captured at flush)
  RqItem* mRqPrev = nullptr;    // previous frame's snapshot (matchAndLerp's Q[N-1])
  int mNCur = 0, mNPrev = 0, mHavePrev = 0;
  void rq_capture(const RqItem* items, int n);      // copy the sorted queue snapshot
  void present_vk(Core* core);                      // build+present the in-between, then the real frame
  int  mDbg = -1;                                    // PSXPORT_DEBUG=fps60 lazy latch

  // ---- STAGE 2: prim matching + lerp (docs/fps60-rework.md "Match+lerp stage") -----------------------
  // Slot A no longer replays Q[N-1] verbatim — it draws lerp(Q[N-1], Q[N], t=mT) per matched prim pair.
  // Match key: primary = (dbg_node, emission-index-within-that-node) provenance, validated on every hit
  // by a shape-fingerprint + per-vertex-color equality check; dbg_node==0 prims (no provenance) fall back
  // to (fingerprint, order-of-appearance) ONLY when the per-fingerprint counts agree between the two
  // frames. All scratch buffers are Fps60 members, cleared/reused every frame — no per-frame `new`.
  float   mT      = 0.5f;         // in-between parameter (t=0.5 for one midpoint at 30->60fps)
  RqItem* mRqLerp = nullptr;      // output buffer for the matched/lerped slot-A queue
  int     mNLerp  = 0;
  std::vector<uint32_t> mIdxPrevBuf, mIdxCurBuf;             // per-item emission-index-within-node (or "no node")
  std::unordered_map<uint32_t, uint32_t> mNodeIdxScratch;    // node -> running emission counter (buildProvenanceIdx scratch)
  std::unordered_map<uint64_t, int> mMatchMap;               // Q[N] provenance key -> item index
  std::unordered_map<uint64_t, std::vector<int>> mZeroGroupsPrev, mZeroGroupsCur;   // dbg_node==0: fingerprint -> ordered indices
  std::vector<int>     mMatchOfA;   // per Q[N-1] item: matched Q[N] index, or -1
  std::vector<uint8_t> mUsedB;      // per Q[N] item: already claimed by a match
  // Tier-3 object-atomicity scratch (docs/fps60-rework.md "QUEUE-LERP ... object-atomic"): per dbg_node,
  // how many Q[N-1] prims that node has vs. how many of them got a Pass-1 match. A node where the two
  // counts disagree is demoted whole — every one of its prims draws unlerped from Q[N-1] — instead of a
  // torn mix of lerped + frozen prims from the same object.
  std::unordered_map<uint32_t, int> mNodeTotalA, mNodeMatchedA;
  long mMatchedThisFrame = 0, mUnmatchedThisFrame = 0;        // this-frame telemetry
  long mMatchedTotal = 0, mUnmatchedTotal = 0;                // running totals (periodic log, PSXPORT_DEBUG=fps60)
  // BACKDROP EXCLUSION (fps60.cpp kBackdropVerbatim): screen-space scroll layer prims, drawn verbatim from
  // Q[N-1] every interp present — never matched, never lerped. Counted separately from matched/unmatched
  // so telemetry distinguishes "never eligible" from "eligible, no confident pair this frame".
  long mBackdropPrimsThisFrame = 0;
  void buildProvenanceIdx(const RqItem* items, int n, std::vector<uint32_t>& out);
  void matchAndLerp(Core* core);   // fills mRqLerp/mNLerp by matching mRqPrev (Q[N-1]) against mRqCur (Q[N])
  void enforceNodeAtomicity(int nA);   // Tier-3: demote a partially-matched dbg_node's prims back to unmatched

  // ---- per-present frame dump (debug channel `fps60dump`, REPL `debug fps60dump`) ---------------------
  // Writes one PNG per PRESENTED frame (real AND interp) to scratch/framedump/, so a Python script can
  // walk the sequence and check whether interpolated frames sit between their neighboring real frames
  // (fps60 correctness) instead of teleporting. Reuses the same VRAM-readback writer as REPL `shot`
  // (gpu_gpu_shot / gpu_native_shot) — no new pixel-readback path. Capped at kDumpMax files so an
  // unbounded run can't fill disk; toggling the channel back on resets the cap.
  static constexpr int kDumpMax = 600;
  int mDumpSeq = 0;
  void dumpPresent(Core* core, bool interp);          // called from present_vk after each present pass
  ~Fps60();
};

#endif // GAME_RENDER_FPS60_H
