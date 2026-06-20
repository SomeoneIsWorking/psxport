// fps60_internal.h — the interpolated-60fps tier's per-instance state (fps60.cpp).
//
// De-globalization (2026-06-19): fps60.cpp's capture buffers, object/transform matcher, SXY remap,
// primitive double-buffer, per-object centroids and rate detector now live on an `Fps60State` instance
// owned by Game (game.h), reached via core->game->fps60. The touching functions are methods of
// Fps60State (field names keep their historical s_ spelling so the bodies are unchanged by the move);
// the public capture API (fps60_rtp/cap_*/join_poly/frame_commit) stays C-style via Core*-taking
// wrappers. The whole tier is RENDER-SIDE (never writes guest RAM) and gated behind PSXPORT_FPS60.
//
// STAYS SHARED (not here): the config-caches s_disp_gate/s_ocen_gate/s_sdbg + the live UI gate
// g_fps60_on (a process-wide mode toggle, edited by the overlay) + function-local diag statics.
#ifndef FPS60_INTERNAL_H
#define FPS60_INTERNAL_H
#include <stdint.h>

struct Core;

#define GW 1024
#define GH 512
#define XOBJ_MAX 1024
#define XV_MAX   80000
#define REMAP_SZ 131072
#define PRIM_MAX 8192
#define OCEN_SZ  8192

// A GTE transform group (camera+model), identified across frames by its local-vertex fingerprint.
typedef struct {
  uint32_t r0, r1, r2, r3, r4;     // rotation matrix, GTE control regs CR0..4 (packed int16 pairs)
  int32_t  trx, try_, trz;         // translation, CR5..7
  uint64_t fp;                     // local-vertex fingerprint = cross-frame identity
  long     nrtps;                  // RTPS/RTPT count (object size)
  int      v0, nv;                 // range [v0,v0+nv) into the per-frame local-vertex pool
} XObj;

// A fully-captured GP0 primitive (frame B's display list, for the in-between synthesizer).
typedef struct {
  uint8_t  op, nv;
  int16_t  x[4], y[4];
  uint8_t  u[4], v[4];
  uint8_t  r[4], g[4], b[4];
  int16_t  w, h;                 // sprite/rect size (nv==1); unused for polys/lines
  int16_t  off_x, off_y;         // E5 draw offset at draw time (the framebuffer origin)
  int16_t  tp_x, tp_y;           // texpage base (px)
  uint8_t  mode, blend, dither;  // texture color mode / semi-transparency mode / ordered-dither
  int16_t  clut_x, clut_y;       // CLUT base (px)
  uint32_t obj;                  // joined object id (0 = unjoined -> snap)
} Prim;

typedef struct { uint32_t obj; int32_t sx, sy, n; } ObjCen;   // per-object screen centroid accumulator
typedef struct { uint64_t last_hash; int held; int period; int votes[9]; long changes; } RateDet;  // logic-rate detector

// ---- Fps60State — the 60fps tier's per-instance render-interp state + methods --------------------
struct Fps60State {
  // object tag (was the cross-TU global g_current_object): the object whose RTP ops are being tagged
  uint32_t current_object = 0;

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

  // transform interpolation + GTE re-projection (old-SXY -> new-SXY remap)
  int     s_xmatch[XOBJ_MAX] = {};
  int32_t s_rm_key[REMAP_SZ] = {};
  int32_t s_rm_val[REMAP_SZ] = {};
  int     s_rm_init = 0;

  // diag: how many RTPS carry an object context (were non-static file-scope globals; fps60-local)
  long s_rtp_calls = 0, s_rtp_with_obj = 0;

  // full primitive capture: PrimFrame A (prev) / B (current)
  Prim  s_prim_a[PRIM_MAX] = {}, s_prim_b[PRIM_MAX] = {};
  Prim* s_pA = s_prim_a;
  Prim* s_pB = s_prim_b;
  int   s_nA = 0, s_nB = 0;
  int   s_overflow = 0;

  // per-object screen-centroid motion (interp key = node pointer)
  ObjCen  s_oc0[OCEN_SZ] = {}, s_oc1[OCEN_SZ] = {};
  ObjCen* s_ocA = s_oc0;
  ObjCen* s_ocB = s_oc1;

  // live 60fps present (1 frame behind) + logic-rate detector
  int     s_prev_front_y = -1;
  RateDet s_rd = { 0, 0, 2, {}, 0 };

  // ---- VK queue-snapshot interpolation (the real 60fps path) ----
  // Snapshot the engine render queue (whole frame: world + 2D) each logic frame; interpolate matched
  // world prims to the A/B midpoint and render the in-between THROUGH VK (not the dead SW s_interp path).
  struct RqItem* s_rqPrev = nullptr;     // previous logic frame's queue snapshot
  struct RqItem* s_rqCur  = nullptr;     // current frame's snapshot (captured at flush)
  struct RqItem* s_rqLerp = nullptr;     // built in-between
  int s_nPrev = 0, s_nCur = 0, s_have_prev = 0;
  void rq_capture(const struct RqItem* items, int n);   // copy the (sorted) queue snapshot
  int  build_lerp();                                    // match cur<->prev, lerp to midpoint; returns count
  void fps60_present_vk(Core* core);                    // emit in-between + real frame, paced (60fps)

  // ---- methods (bodies in fps60.cpp; reached via core->game->fps60) ----
  void     fold(uint32_t v);
  void     grid_put(int sx, int sy, uint32_t obj);
  uint32_t grid_get(int px, int py);
  void     xvert(int16_t vx, int16_t vy, int16_t vz, uint32_t sxy);
  void     xobj_rtp(uint32_t insn);
  void     xobj_report();
  void     xobj_commit();
  void     xobj_match();
  void     remap_reset();
  void     remap_put(int32_t key, int32_t val);
  int      remap_get(int32_t key, int32_t* out);
  void     fps60_build_remap();
  void     rtp(uint32_t op);
  void     join_poly(int px, int py);
  void     cap_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                    const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                    int off_x, int off_y, int tp_x, int tp_y, int mode, int blend, int dither,
                    int clut_x, int clut_y);
  void     cap_sprite(int op, int x, int y, int u, int v, int w, int h,
                      int r, int g, int b, int off_x, int off_y,
                      int tp_x, int tp_y, int mode, int blend, int clut_x, int clut_y);
  void     cap_line(int op, int x0, int y0, int x1, int y1, int r, int g, int b, int semi);
  int      fps60_front_off_y();
  int      ocen_delta(uint32_t obj, int* dx, int* dy);
  long     fps60_synthesize(Core* core);
  void     fps60_synth_dumptest(Core* core);
  void     fps60_present(Core* core);
  void     frame_commit(Core* core);
};

#endif // FPS60_INTERNAL_H
