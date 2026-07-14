// game/render/fps60.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// RENDERER-INTERNAL, one-frame-behind interpolation (docs/fps60-rework.md, redesign 2026-07-14). The
// renderer holds the last TWO real frames' resolved render-queue prims (double-buffered by rq_capture) and
// presents one frame behind: slot A draws lerp(Q[N-1], Q[N], t=0.5) via matchAndLerp's provenance-matched
// vertex lerp, slot B draws Q[N] verbatim. ONE draw path — both slots drain an RqItem list through the same
// `RenderQueue::emitItem` the 30fps path uses. NO guest reads at present time: no sceneNative re-run, no
// camera reprojection, no per-object/billboard anchor stamping (that whole design — object-transform
// midpoint providers, per-billboard world anchors — is RETIRED; see docs/fps60-rework.md stage 3).
//
// The match heuristic (matchAndLerp) is transitional: docs/fps60-rework.md's "REDIRECT" flags it as hack
// debt to be replaced, emitter by emitter, by real per-element identity as each quad-emitting guest fn is
// RE'd and ported native — it is NOT deleted yet.
//
// HOST-ONLY (the READ-ONLY OVERLAY invariant): every capture is a guest READ (at queue-flush time, not
// present time); every store is host memory. When g_mods.fps60 is off, none of this arms and the 30fps
// path is byte-identical.
//
// De-globalization (2026-06-19): all state lives on this Fps60 instance owned by Game (game.h), reached
// via core->game->fps60; every touching function is a method (callers use c->game->fps60.method(...)).
#ifndef GAME_RENDER_FPS60_H
#define GAME_RENDER_FPS60_H
#include <stdint.h>
#include <unordered_map>
#include <vector>

struct Core;
struct RqItem;
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
  long mMatchedThisFrame = 0, mUnmatchedThisFrame = 0;        // this-frame telemetry
  long mMatchedTotal = 0, mUnmatchedTotal = 0;                // running totals (periodic log, PSXPORT_DEBUG=fps60)
  void buildProvenanceIdx(const RqItem* items, int n, std::vector<uint32_t>& out);
  void matchAndLerp(Core* core);   // fills mRqLerp/mNLerp by matching mRqPrev (Q[N-1]) against mRqCur (Q[N])

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
