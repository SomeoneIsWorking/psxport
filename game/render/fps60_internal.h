// fps60_internal.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// De-globalization (2026-06-19): fps60.cpp's RTP-tap capture buffers, the render-queue snapshots and
// the rate detector live on an `Fps60` instance owned by Game (game.h), reached via core->game->
// fps60. All external touching functions ARE methods of Fps60 (field names keep their historical s_
// spelling); callers reach them directly as `c->game->fps60.rtp(op)` etc. — no free-function
// wrappers. The whole tier is RENDER-SIDE (never writes guest RAM) and gated behind PSXPORT_FPS60.
//
// STAYS SHARED (not here): the config-caches s_disp_gate/s_ocen_gate/s_sdbg + the live UI gate
// g_mods.fps60 (the overlay-edited persistent user setting) + function-local diag statics.
#ifndef FPS60_INTERNAL_H
#define FPS60_INTERNAL_H
#include <stdint.h>

struct Core;

#define GW 1024
#define GH 512
#define XOBJ_MAX 1024
#define XV_MAX   80000

// A GTE transform group (camera+model), identified across frames by its local-vertex fingerprint.
typedef struct {
  uint32_t r0, r1, r2, r3, r4;     // rotation matrix, GTE control regs CR0..4 (packed int16 pairs)
  int32_t  trx, try_, trz;         // translation, CR5..7
  uint64_t fp;                     // local-vertex fingerprint = cross-frame identity
  long     nrtps;                  // RTPS/RTPT count (object size)
  int      v0, nv;                 // range [v0,v0+nv) into the per-frame local-vertex pool
} XObj;

typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;  // logic-rate detector

// ---- Fps60 — the 60fps tier's per-instance render-interp state + methods --------------------
struct Fps60 {
  // object tag (was the cross-TU global g_current_object): the object whose RTP ops are being tagged
  uint32_t current_object = 0;

  // fps60 actor key: the per-object render command (cmd ptr) whose world quads are CURRENTLY being
  // submitted (set by submit_perobj_flush around native_dispatch). Stamped onto each captured world
  // RqItem (fps_key) so the 60fps tier can match an actor's prims across frames and reproject them at
  // the A/B transform midpoint. 0 = no actor context (the prim snaps).
  uint32_t fps_cur_key = 0;

  // per-frame projected-geometry fingerprint (rate-detector input)
  uint64_t s_frame_hash = 1469598103934665603ull;
  long     s_frame_geom = 0;
  long     s_fence = 0;

  // SXY -> object-id grid (the join), epoch-stamped
  uint32_t s_obj_grid[GW * GH] = {};
  uint32_t s_obj_stamp[GW * GH] = {};
  uint32_t s_epoch = 0;
  long     s_join_hit = 0, s_join_miss = 0;

  // native graphical objects (GTE transform groups) + per-frame local-vertex pool
  XObj  s_xa[XOBJ_MAX] = {}, s_xb[XOBJ_MAX] = {};
  XObj* s_xA = s_xa;          // previous frame's objects
  XObj* s_xB = s_xb;          // current frame's objects (capturing)
  int   s_nxA = 0, s_nxB = 0;
  int   s_xb_started = 0;
  int16_t s_lvx[XV_MAX] = {}, s_lvy[XV_MAX] = {}, s_lvz[XV_MAX] = {};
  int32_t s_osxy[XV_MAX] = {};
  int   s_nv = 0;
  uint32_t s_rtps_insn = 0x00080001;

  // diag: how many RTPS carry an object context (were non-static file-scope globals; fps60-local)
  long s_rtp_calls = 0, s_rtp_with_obj = 0;

  // logic-rate detector
  RateDet s_rd = { 0, 0, 2, {}, 0 };

  // ---- VK queue-snapshot interpolation (the real 60fps path) ----
  // Snapshot the engine render queue (whole frame: world + 2D) each logic frame; interpolate matched
  // world prims to the A/B midpoint and render the in-between THROUGH VK.
  struct RqItem* s_rqPrev = nullptr;     // previous logic frame's queue snapshot
  struct RqItem* s_rqCur  = nullptr;     // current frame's snapshot (captured at flush)
  struct RqItem* s_rqLerp = nullptr;     // built in-between
  int s_nPrev = 0, s_nCur = 0, s_have_prev = 0;
  void rq_capture(const struct RqItem* items, int n);   // copy the (sorted) queue snapshot
  int  build_lerp();                                    // match cur<->prev, lerp to midpoint; returns count
  void fps60_present_vk(Core* core);                    // emit in-between + real frame, paced (60fps)
  // PSXPORT_DEBUG=fps60pass — prove the two 60fps presents emit the SAME COMPLETE frame: count the
  // HUD-layer items and the shadow-casting prims in a queue set (was the file-scope fps60_pass_stats).
  void pass_stats(const char* tag, long fence, const struct RqItem* items, int n);
  int  s_lerpdbg = -1;                   // PSXPORT_DEBUG=fps60 — per-frame reproject stats (lazy latch)
  int  s_chk = -1;                       // PSXPORT_DEBUG=fps60chk — mechanical t=1.0 reproject gate
  int  s_passdbg = -1;                   // PSXPORT_DEBUG=fps60pass latch
  int* s_bgdx = nullptr;                 // build_lerp per-tile bg displacement scratch (FPS60_RQ_MAX)
  int* s_bgdy = nullptr;
  ~Fps60() { delete[] s_bgdx; delete[] s_bgdy; }

  // ---- billboard registry (was file-scope s_bbCur / s_nBBCur in fps60.cpp) --------
  // Per-Core so SBS's two cores don't share the same billboard registry between their emits
  // (engine_submit calls record_billboard_span from THIS core's frame; the OT walk reads it
  // via billboard_for_node — must be same core's) (deglobalize 2026-07-03).
  static constexpr int kBbMax = 1024;
  struct Billboard { uint32_t lo, hi; uint32_t ident; uint32_t crM[11]; };
  Billboard s_bbCur[kBbMax] = {};
  int       s_nBBCur = 0;

  // ---- methods (bodies in fps60.cpp; reached via core->game->fps60) ----
  void     fold(uint32_t v);
  void     grid_put(int sx, int sy, uint32_t obj);
  uint32_t grid_get(int px, int py);
  void     xvert(int16_t vx, int16_t vy, int16_t vz, uint32_t sxy);
  void     xobj_rtp(uint32_t insn);
  void     xobj_commit();
  void     rtp(uint32_t op);
  void     join_poly(int px, int py);
  void     frame_commit(Core* core);
  // billboard-registry methods (former file-scope free fns)
  void bbFrameReset() { s_nBBCur = 0; }
  void recordBillboardSpan(uint32_t lo, uint32_t hi, uint32_t ident);
  int  billboardForNode(uint32_t node, uint32_t* identOut, uint32_t crOut[11]) const;
};

#endif // FPS60_INTERNAL_H
