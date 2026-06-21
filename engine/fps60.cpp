#include "core.h"
#include "game.h"   // Fps60State (per-instance render-interp state) via core->game->fps60
extern "C" {  // Beetle GTE (mednafen gte.c, compiled as C)
  uint32_t GTE_ReadDR(unsigned); uint32_t GTE_ReadCR(unsigned);
  void GTE_WriteDR(unsigned, uint32_t); void GTE_WriteCR(unsigned, uint32_t);
  int32_t GTE_Instruction(uint32_t);
}
// fps60 — interpolated-60fps tier for the native PC port (design: docs/fps60_recomp_60fps.md).
//
// This file owns the capture buffers, the logic-rate detector, the object→primitive join, and
// (later) the matcher + in-between synthesizer. GATED behind PSXPORT_FPS60 and purely additive:
// when off, the taps in gte_beetle.c / gpu_native.c / games_tomba2.c are no-ops and the faithful
// 4:3/30fps path is byte-identical.
//
// MILESTONE 1 (done): measured Tomba2's logic rate = 30 fps (period 1, quota 2) → one in-between/frame.
// MILESTONE 2 (this commit): the OBJECT→PRIMITIVE JOIN — the matcher's foundation and the design's
//   #1 open risk ("re-validate on the native path"). Tag every RTP-produced SXY with the current
//   object id (from the 0x8007712C cull dispatcher), then at draw time join each GP0 polygon to a
//   captured SXY by vertex coords (±2px). Report the join rate: the fraction of drawn polys that are
//   object-matchable (3D models) vs. unjoinable (CPU-projected terrain / 2D HUD → will snap).
#include <stdint.h>
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


int g_fps60_on = 0;          // read by the gte_op tap; set by fps60_init from PSXPORT_FPS60

// ---- per-frame projected-geometry fingerprint (rate detector input) -----------------

void Fps60State::fold(uint32_t v) {
  uint64_t h = s_frame_hash;
  for (int i = 0; i < 4; i++) { h ^= (v & 0xFF); h *= 1099511628211ull; v >>= 8; }
  s_frame_hash = h; s_frame_geom++;
}

// ---- SXY → object-id grid (the join) -------------------------------------------------
// Last object that projected a vertex to each (sx,sy). Epoch-stamped so it resets per frame with
// no 2 MB memset: a cell is live only when its stamp == the current epoch.

void Fps60State::grid_put(int sx, int sy, uint32_t obj) {
  int x = sx & (GW - 1), y = sy & (GH - 1);
  int i = y * GW + x;
  s_obj_grid[i] = obj; s_obj_stamp[i] = s_epoch;
}
uint32_t Fps60State::grid_get(int px, int py) {  // ±2px search; returns the source object's node pointer
  for (int dy = -2; dy <= 2; dy++)          // (0 = no object near here / CPU-projected → caller snaps)
    for (int dx = -2; dx <= 2; dx++) {
      int x = (px + dx) & (GW - 1), y = (py + dy) & (GH - 1);
      int i = y * GW + x;
      if (s_obj_stamp[i] == s_epoch && s_obj_grid[i]) return s_obj_grid[i];
    }
  return 0;
}


// ---- native graphical objects (GTE transform groups) ---------------------------------
// The game's camera + models, ported to native: every run of RTPS/RTPT sharing one GTE transform
// (rotation matrix CR0-4 + translation CR5-7 = the model-view, camera baked in) is ONE object. Its
// identity ACROSS frames is its local-vertex fingerprint (the model mesh is invariant; only the
// transform moves). Interpolating each object's transform between frames and re-projecting its verts
// reproduces camera pan + object motion perspective-correctly. (RE: the 0x8007712C cull dispatcher does
// NOT tag these scenes — the GTE transform is the real object identity.)
// Per-frame local-vertex pool for the CURRENT frame (B): each captured vertex's model-space coords and
// the screen XY the GTE produced for it (the key we remap on). Rebuilt every frame.

static void xfold(XObj* o, uint32_t v) {     // fold a local-vertex word into the fingerprint
  o->fp ^= v + 0x9E3779B97F4A7C15ull + (o->fp << 6) + (o->fp >> 2);
}
void Fps60State::xvert(int16_t vx, int16_t vy, int16_t vz, uint32_t sxy) {
  if (s_nv >= XV_MAX) return;
  s_lvx[s_nv] = vx; s_lvy[s_nv] = vy; s_lvz[s_nv] = vz; s_osxy[s_nv] = (int32_t)sxy; s_nv++;
}

// Called per RTPS(0x01)/RTPT(0x30) from fps60_rtp, with the GTE holding this vertex's transform.
void Fps60State::xobj_rtp(uint32_t insn) {
  uint32_t op = insn & 0x3F;
  if (op == 0x01) s_rtps_insn = insn;          // remember the game's RTPS flags for re-projection
  uint32_t r0 = GTE_ReadCR(0), r1 = GTE_ReadCR(1), r2 = GTE_ReadCR(2), r3 = GTE_ReadCR(3), r4 = GTE_ReadCR(4);
  int32_t  t5 = (int32_t)GTE_ReadCR(5), t6 = (int32_t)GTE_ReadCR(6), t7 = (int32_t)GTE_ReadCR(7);
  XObj* o = s_xb_started ? &s_xB[s_nxB - 1] : NULL;
  int same = o && o->r0==r0 && o->r1==r1 && o->r2==r2 && o->r3==r3 && o->r4==r4
               && o->trx==t5 && o->try_==t6 && o->trz==t7;
  if (!same) {                                  // transform changed → new object
    if (s_nxB >= XOBJ_MAX) return;
    o = &s_xB[s_nxB++]; s_xb_started = 1;
    o->r0=r0; o->r1=r1; o->r2=r2; o->r3=r3; o->r4=r4; o->trx=t5; o->try_=t6; o->trz=t7;
    o->fp = 1469598103934665603ull; o->nrtps = 0; o->v0 = s_nv; o->nv = 0;
  }
  // Capture LOCAL input verts (model-space, frame-invariant) + each one's screen output, and fold verts
  // into the cross-frame fingerprint. RTPS: 1 vert (DR0/1) → SXY DR14. RTPT: 3 verts (DR0/1,2/3,4/5) →
  // SXY DR12,DR13,DR14 in order.
  uint32_t v0 = GTE_ReadDR(0); int16_t z0 = (int16_t)GTE_ReadDR(1);
  xfold(o, v0); xfold(o, (uint16_t)z0);
  if (op == 0x30) {
    uint32_t v1 = GTE_ReadDR(2); int16_t z1 = (int16_t)GTE_ReadDR(3);
    uint32_t v2 = GTE_ReadDR(4); int16_t z2 = (int16_t)GTE_ReadDR(5);
    xfold(o, v1); xfold(o, (uint16_t)z1); xfold(o, v2); xfold(o, (uint16_t)z2);
    xvert((int16_t)(v0 & 0xFFFF), (int16_t)(v0 >> 16), z0, GTE_ReadDR(12));
    xvert((int16_t)(v1 & 0xFFFF), (int16_t)(v1 >> 16), z1, GTE_ReadDR(13));
    xvert((int16_t)(v2 & 0xFFFF), (int16_t)(v2 >> 16), z2, GTE_ReadDR(14));
    o->nv += 3;
  } else {
    xvert((int16_t)(v0 & 0xFFFF), (int16_t)(v0 >> 16), z0, GTE_ReadDR(14));
    o->nv += 1;
  }
  o->nrtps++;
}

void Fps60State::xobj_commit() {                 // swap A/B at frame end; reset the per-frame vert pool
  XObj* t = s_xA; s_xA = s_xB; s_xB = t; s_nxA = s_nxB; s_nxB = 0; s_xb_started = 0; s_nv = 0;
}

// average the two packed int16 halves of two GTE matrix control registers (used by fps60_compose_mid)
static uint32_t interp_packed(uint32_t a, uint32_t b) {
  int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
  int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
  uint16_t lo = (uint16_t)(((int)al + bl) / 2), hi = (uint16_t)(((int)ah + bh) / 2);
  return lo | ((uint32_t)hi << 16);
}

// gte_op RTP tap. op 0x01 = RTPS (one new SXY, DR14); 0x30 = RTPT (three, DR12/13/14).
void Fps60State::rtp(uint32_t op) {
  if (!g_fps60_on) return;
  s_rtp_calls++; if (current_object) s_rtp_with_obj++;
  xobj_rtp(op);                 // capture this vertex's GTE transform-group (native object)
  unsigned lo = (op == 0x30) ? 12 : 14, hi = 14;
  for (unsigned r = lo; r <= hi; r++) {
    uint32_t sxy = GTE_ReadDR(r);
    fold(sxy);
    int16_t sx = (int16_t)(sxy & 0xFFFF), sy = (int16_t)(sxy >> 16);
    grid_put(sx, sy, current_object);
  }
}

// gp0_exec polygon tap: join the packet's lead vertex to a captured SXY.
void Fps60State::join_poly(int px, int py) {
  if (!g_fps60_on) return;
  if (grid_get(px, py)) s_join_hit++; else s_join_miss++;
}

// ---- logic-rate detector (lrate_proto.c, validated) ---------------------------------

static void rate_tick(RateDet* d, uint64_t set_hash) {
  if (set_hash == d->last_hash) { d->held++; return; }
  int p = d->held + 1;
  if (p >= 1 && p <= 8) d->votes[p]++;
  int best = 0, bp = 2;
  for (int i = 1; i <= 8; i++) if (d->votes[i] > best) { best = d->votes[i]; bp = i; }
  d->period = bp;
  d->last_hash = set_hash; d->held = 0; d->changes++;
}

// ---- per-logic-frame fence (games_tomba2.c ov_frame_update) -------------------------
void Fps60State::frame_commit(Core* core) {
  if (!g_fps60_on) return;
  uint64_t set_hash = (s_frame_geom > 0) ? s_frame_hash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&s_rd, set_hash);
  s_fence++;

  // The live in-between is built by fps60_present_vk → build_lerp (the ACTOR-TRANSFORM reprojection path
  // below). The legacy GTE-SXY remap and the SW re-rasterizer synth were RETIRED and removed. xobj_commit is
  // kept — it resets the per-frame local-vertex pool that the RTP tap (rtp→xobj_rtp) still appends to, so
  // without it s_nv would grow until XV_MAX and freeze the capture.
  xobj_commit();                                 // reset the per-frame local-vertex pool (RTP-tap hygiene)
  fps60_present_vk(core);       // owns presentation: VK 60fps pair (interpolated in-between + real frame)

  s_frame_hash = 1469598103934665603ull;
  s_frame_geom = 0;
  s_epoch++;                  // reset the SXY→obj grid for the next frame
}


// ============================================================================================
// VK 60fps: render-queue-snapshot interpolation (the LIVE path; replaced the retired SW re-rasterizer).
// Each logic frame the engine render queue (the WHOLE frame — world + 2D, already engine-sorted) is
// captured here (render_queue.cpp's flush snapshots instead of emitting when fps60 is on). We match each
// current world prim to the previous frame's by a position-INDEPENDENT material/UV/color fingerprint
// (nearest centroid as tiebreak), lerp matched prims' screen verts + depth to the A/B midpoint, render
// the in-between THROUGH the normal VK emit path + present it, then render+present the real frame — a
// 60fps stream (in-between, real, ...). Static prims (terrain/HUD) have zero motion -> unchanged; moving
// objects + camera-panned geometry interpolate; teleports/cuts exceed the gate -> snap (no smear).
#include "render_queue.h"
#include "game.h"
#include <unordered_map>
#include <algorithm>
#include <math.h>
void gpu_present_ex(Core* core, int do_blit);
void gpu_fps60_present_pass(Core* core);
void gpu_pace_subframe(Core* core, int n);
// fps60 midpoint reprojection: project model verts through an explicit composed transform (gte_beetle.cpp).
void proj_native_xform_cr(const uint32_t cr[11], const int16_t mv[4][3], int nv, float px[4], float py[4], float pz[4]);
float proj_pz_to_ord(float pz);

#define FPS60_RQ_MAX 16384

// ---- ACTOR-TRANSFORM 60fps tier (user spec 2026-06-21) -------------------------------------------------
// The interpolated in-between is built by REPROJECTING each captured world prim under the A/B MIDPOINT of
// its source actor's transform — NOT by matching screen prims (the retired fingerprint matcher exploded on
// real meshes) and NOT by re-running any guest/interpreted render code (unsafe: the field's per-mode
// renderers mutate guest packet RAM). At native projection (fps60_stamp_world) every GTE-composed world
// quad records its MODEL verts, its composed transform CR0-7, and its actor key (the per-object render
// command). The composed transform already bakes in the camera, so lerping it per actor reproduces BOTH
// camera pan (a static actor's transform differs frame-to-frame only by the camera) AND object motion in
// one mechanism — exactly the user's "static objects move via the interpolated camera only; movers also
// get object-motion interp", with the per-actor transform delta as the static/mover signal (no separate
// flag). Terrain (separate float projection) + 2D/HUD carry fps_world=0 and snap (documented follow-up).

// Capture hook: stamp the just-pushed world RqItem with this prim's reprojection inputs. Called by the GTE
// submitters (engine_submit.cpp) right after gpu_draw_world_quad. No-op unless fps60 is on.
void fps60_stamp_world(Core* c, const int16_t mv[4][3], int nv, uint32_t key) {
  RenderQueue& q = c->game->rq;
  if (q.consumed || q.n == 0) return;                 // the world quad wasn't actually queued
  RqItem* it = &q.items[q.n - 1];
  if (it->layer != RQ_WORLD) return;
  it->fps_world = 1; it->fps_anchor = 0; it->fps_key = key;   // mesh prim: per-vertex reproject (not anchor-translate)
  for (int i = 0; i < 8; i++) it->fps_cr[i] = GTE_ReadCR(i);   // composed camera×object transform CR0-7
  it->fps_cr[8] = GTE_ReadCR(24); it->fps_cr[9] = GTE_ReadCR(25); it->fps_cr[10] = GTE_ReadCR(26); // OFX/OFY/H
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    it->fps_mv[k][0] = mv[s][0]; it->fps_mv[k][1] = mv[s][1]; it->fps_mv[k][2] = mv[s][2]; }
  it->fps_offx = (int16_t)c->game->gpu.s_off_x;       // draw offset baked into xs/ys (reproject reproduces)
  it->fps_offy = (int16_t)c->game->gpu.s_off_y;
}

// ---- 3D-POSITIONED 2D QUAD (billboard) registry — keyed by OBJECT IDENTITY (node-span) ----------------
// The collectable/flame/decal billboards are guest GP0 quads the per-object renderers emit; they enter the
// render queue at the DEFERRED OT walk (gpu_native.cpp) as RQ_WORLD/RQ_OM_DEPTH items, inheriting the
// object's PC-native world view-Z via obj_depth_lookup, but carry NO fps60 mesh capture (fps_world=0,
// has_xyf=0) — so without help the 60fps tier snapped them to camera-B and they juddered while Tomba moved.
//
// Now that Phase 1 gives each billboard an engine-known world position (its object's composed camera×object
// transform), we capture THAT directly and key the cross-frame match on the object's STABLE IDENTITY (the
// node/cmd ptr), not on a fragile depth-ord reverse-match. engine_submit.cpp records, at the SAME instant it
// publishes the object's packet-pool depth span, an entry holding: the span [lo,hi) (so the OT walk can find
// it by the SAME node-address test obj_depth_lookup uses), the object identity, and the live composed
// transform. The OT walk knows the source OT-node (s_cur_node); when a billboard item is queued there it
// calls fps60_billboard_for_node(node, ...) and, on a hit, stamps the item DIRECTLY as an anchor-reproject
// billboard (fps_world=1, fps_anchor=1, fps_key=ident, fps_cr=transform, fps_mv=object origin). No build_lerp
// pre-pass, no depth-ord matching. The previous frame's transform is found by fps_key out of the prev
// RqItems (which now carry it), exactly like the mesh path — so the s_bbPrev registry is no longer needed
// to hold prev transforms; the registry is purely the CURRENT-frame node→transform map for queue-time tagging.
// File-scope (not on Fps60State — fps60_internal.h is not ours to edit); the 60fps present path is single-
// core in practice, like the s_lerpdbg/s_chk statics. Host-only; never touches guest RAM.
struct Fps60Billboard { uint32_t lo, hi; uint32_t ident; uint32_t crM[11]; };
#define FPS60_BB_MAX 1024
static Fps60Billboard s_bbCur[FPS60_BB_MAX];
static int s_nBBCur = 0;
// The billboard-identity registry is needed both by the 60fps reproject AND by the `debug objid` overlay
// (which labels billboards with their object id in any mode). So record/lookup/stamp whenever EITHER is
// active. In 60fps-off mode the registry is reset per frame from the render queue (fps60_bb_frame_reset,
// called from RenderQueue::push); the 60fps path resets it in frame_commit as before.
static inline int bb_active(void) { return g_fps60_on || cfg_dbg("objid"); }
void fps60_bb_frame_reset(void) { s_nBBCur = 0; }
// (GTE_ReadCR is declared in the extern "C" block at the top of this file.)

// Called from engine_submit.cpp right after gpu_obj_depth_add(lo, hi, ord). Capture this object's billboard
// reprojection inputs: its packet-pool SPAN [lo,hi) (the key the OT walk matches the item's node against,
// identical to GpuState::obj_depth_lookup), its stable cross-frame IDENTITY, and the live composed
// camera×object transform (CR0-7 + the frame projection constants CR24/25/26). No-op unless fps60 is on.
void fps60_record_billboard_span(Core* /*c*/, uint32_t lo, uint32_t hi, uint32_t ident) {
  if (!bb_active() || !ident || hi <= lo) return;
  if (s_nBBCur >= FPS60_BB_MAX) return;
  Fps60Billboard* e = &s_bbCur[s_nBBCur++];
  e->lo = lo | 0x80000000u; e->hi = hi | 0x80000000u; e->ident = ident;   // KSEG, matching s_cur_node form
  for (int i = 0; i < 8; i++) e->crM[i] = GTE_ReadCR(i);            // composed camera×object CR0-7 (live)
  e->crM[8] = GTE_ReadCR(24); e->crM[9] = GTE_ReadCR(25); e->crM[10] = GTE_ReadCR(26);  // OFX/OFY/H
}

// Queue-time lookup (called from the OT walk in gpu_native.cpp): if `node` falls in a recorded billboard
// span this frame, return its identity + composed transform so the caller can stamp the RqItem as an
// anchor-reproject billboard. Returns 1 on hit. node is the OT-node address (KSEG, == s_cur_node|0x80000000).
static int fps60_billboard_for_node(uint32_t node, uint32_t* ident, uint32_t cr[11]) {
  if (!bb_active()) return 0;
  uint32_t n = node | 0x80000000u;
  for (int i = 0; i < s_nBBCur; i++)
    if (n >= s_bbCur[i].lo && n < s_bbCur[i].hi) {
      if (ident) *ident = s_bbCur[i].ident;
      if (cr) for (int k = 0; k < 11; k++) cr[k] = s_bbCur[i].crM[k];
      return 1;
    }
  return 0;
}

// Stamp the JUST-PUSHED billboard RqItem as an anchor-reproject billboard. Called from the OT walk
// (gpu_native.cpp) right after a 2D billboard prim (a world-band, obj_depth-hit quad) is queued, with the
// prim's source OT-node address. If that node falls in a recorded billboard span this frame, we set the same
// fps fields the old build_lerp pre-pass set, but keyed on the object's IDENTITY (node ptr) directly:
//   fps_world=1, fps_anchor=1, fps_key=ident, fps_cr=composed transform, fps_mv=object origin (0,0,0).
// build_lerp then reprojects the WORLD ANCHOR (the origin under the composed camera×object transform) at the
// midpoint and translates the whole 2D quad — so the billboard pans/moves smoothly instead of snapping to
// camera-B. The prev-frame transform is found by fps_key out of the prev RqItems (which now carry it).
// No-op unless fps60 is on / no span matches. node = OT-node address (the gp0 walk's s_cur_node).
void fps60_stamp_billboard(Core* c, uint32_t node) {
  if (!bb_active()) return;
  RenderQueue& q = c->game->rq;
  if (q.consumed || q.n == 0) return;
  RqItem* it = &q.items[q.n - 1];
  if (it->fps_world) return;                            // already a native-mesh capture
  if (it->layer != RQ_WORLD || it->order_mode != RQ_OM_DEPTH) return;
  uint32_t ident; uint32_t cr[11];
  if (!fps60_billboard_for_node(node, &ident, cr)) return;
  it->fps_world = 1; it->fps_anchor = 1; it->fps_key = ident;
  for (int k = 0; k < 11; k++) it->fps_cr[k] = cr[k];
  for (int k = 0; k < 4; k++) { it->fps_mv[k][0] = 0; it->fps_mv[k][1] = 0; it->fps_mv[k][2] = 0; }  // anchor = object origin (CR5-7 = view pos)
  it->fps_offx = 0;   // billboard reproject is a screen TRANSLATE of xs/ys (offset already baked in)
  it->fps_offy = 0;
}

static int s_lerpdbg = -1;        // PSXPORT_DEBUG=fps60 — per-frame reproject stats

void Fps60State::rq_capture(const RqItem* items, int n) {
  if (!s_rqCur) s_rqCur = new RqItem[FPS60_RQ_MAX];
  if (n > FPS60_RQ_MAX) n = FPS60_RQ_MAX;
  if (n > 0) memcpy(s_rqCur, items, (size_t)n * sizeof(RqItem));
  s_nCur = n;
}

// Max per-VERTEX screen motion (px, L1) we will treat as "the same vertex moved a little" and lerp.
// This is the load-bearing robustness gate, not the centroid one. Justification: Tomba2 logic runs at
// 30fps and the in-between is the t=0.5 midpoint, so a vertex's true frame-to-frame motion is at most
// ~half the on-screen speed of the fastest thing we interpolate. World geometry (camera pan + model
// motion) moves at most a few tens of px/frame at this rate; 48px L1 comfortably covers genuine motion
// while rejecting the cross-vertex pairings that cause the explosion (a mis-paired vertex jumps a large
// fraction of the screen). A vertex whose paired displacement exceeds this is NOT the same vertex (wrong
// fingerprint collision, permuted winding, or a real teleport/cut) -> we must not average it.
#define FPS60_VTX_GATE 48

// Reproject one captured world prim under crM (its actor's A/B-midpoint composed transform) into `out`.
// Returns the worst per-vertex L1 screen displacement from the prim's real (frame-B) position — the caller
// snaps instead when this exceeds the gate (teleport/cut/bad match → no smear). Reproduces the exact
// round+offset gpu_draw_world_quad applied, so a crM == fps_cr (t=1.0) reprojection is bit-faithful.
static int fps60_reproject(const RqItem* C, const uint32_t crM[8], RqItem* out) {
  float px[4], py[4], pz[4];
  proj_native_xform_cr(crM, C->fps_mv, C->nv, px, py, pz);
  int worst = 0;
  for (int k = 0; k < C->nv; k++) {
    int xs = (int)(px[k] < 0 ? px[k] - 0.5f : px[k] + 0.5f) + C->fps_offx;
    int ys = (int)(py[k] < 0 ? py[k] - 0.5f : py[k] + 0.5f) + C->fps_offy;
    int d = abs(xs - C->xs[k]) + abs(ys - C->ys[k]);
    if (d > worst) worst = d;
    out->xs[k] = xs; out->ys[k] = ys; out->depth[k] = proj_pz_to_ord(pz[k]);
    // Vertex smoothing (#15): also carry the SUB-PIXEL reprojected XY so the 60fps in-between is smooth
    // too (the gate above still uses the integer position). has_xyf came along in the *C copy.
    out->xsf[k] = px[k] + (float)C->fps_offx;
    out->ysf[k] = py[k] + (float)C->fps_offy;
  }
  return worst;
}

// Reproject a 3D-positioned 2D quad (billboard) under crT (its actor's transform at parameter T): project the
// captured WORLD-POSITION anchor (fps_mv[0], the object origin under the composed camera×object transform) to a
// screen point, then TRANSLATE the whole quad by (anchor_T - anchor_B). The quad keeps its screen-space size +
// orientation (it is a billboard) and only its anchor moves — exactly right for a sprite pinned to a world point.
// Returns the worst per-vertex L1 displacement from frame-B (= |anchor delta|, since it is a uniform shift), so
// the caller still snaps a billboard that jumps more than the gate (teleport/cut). crT==fps_cr (t=1.0) → zero
// delta → bit-faithful (the fps60chk gate stays 0px for billboards too).
static int fps60_reproject_anchor(const RqItem* C, const uint32_t crT[11], RqItem* out) {
  float ax[4], ay[4], az[4];
  proj_native_xform_cr(crT, C->fps_mv, 1, ax, ay, az);            // anchor under crT (1 vert = the origin)
  float axB[4], ayB[4], azB[4];
  proj_native_xform_cr(C->fps_cr, C->fps_mv, 1, axB, ayB, azB);   // anchor under the prim's own (frame-B) transform
  float dxf = ax[0] - axB[0], dyf = ay[0] - ayB[0];              // sub-pixel screen translation of the anchor
  int dxi = (int)(dxf < 0 ? dxf - 0.5f : dxf + 0.5f);
  int dyi = (int)(dyf < 0 ? dyf - 0.5f : dyf + 0.5f);
  for (int k = 0; k < C->nv; k++) {
    out->xs[k] = C->xs[k] + dxi; out->ys[k] = C->ys[k] + dyi;     // shift the integer 2D quad
    out->xsf[k] = C->xsf[k] + dxf; out->ysf[k] = C->ysf[k] + dyf; // and the sub-pixel copy (smooth)
    out->depth[k] = C->depth[k];                                  // billboard depth band unchanged
  }
  return abs(dxi) + abs(dyi);                                     // uniform shift → this IS the worst per-vertex L1
}

// Midpoint of an actor's composed transform: rotation CR0-4 = packed-int16 average, translation CR5-7 =
// integer average. (Small-angle matrix lerp; refine to euler-slerp if fast spins warp — see journal.)
static void fps60_compose_mid(const uint32_t a[11], const uint32_t b[11], uint32_t m[11]) {
  for (int k = 0; k < 5; k++) m[k] = interp_packed(a[k], b[k]);                 // rotation CR0-4 (packed i16)
  for (int k = 5; k < 8; k++) m[k] = (uint32_t)(int32_t)(((int64_t)(int32_t)a[k] + (int32_t)b[k]) / 2);  // trans
  for (int k = 8; k < 11; k++) m[k] = (uint32_t)(int32_t)(((int64_t)(int32_t)a[k] + (int32_t)b[k]) / 2);  // OFX/OFY/H
}

// Build the interpolated in-between by reprojecting each world prim at its actor's transform midpoint.
// A static actor's transform differs across frames only by the camera → it reprojects under the half-camera
// (correct pan); a mover's also by its own motion → midpoint pose. 2D/HUD, terrain, unkeyed or new-this-
// frame actors, and any prim whose reprojection jumps more than the gate (cut/teleport) SNAP unchanged.
int Fps60State::build_lerp() {
  if (!s_rqLerp) s_rqLerp = new RqItem[FPS60_RQ_MAX];
  // Billboards are now tagged at QUEUE TIME by the OT walk (gpu_native.cpp → fps60_billboard_for_node), so a
  // billboard RqItem already carries fps_world=1/fps_anchor=1/fps_key/fps_cr/fps_mv=origin on BOTH the cur and
  // prev snapshots — exactly like a mesh prim. No pre-pass and no separate prev-transform registry are needed.
  // actor key -> its composed CR0-7 LAST frame (all of an actor's prims — mesh OR billboard — share one
  // composed transform, so first occurrence wins). Both mesh and billboard prev transforms live in s_rqPrev.
  std::unordered_map<uint32_t, const uint32_t*> prevcr;
  prevcr.reserve((size_t)s_nPrev * 2 + 16);
  for (int j = 0; j < s_nPrev; j++) { const RqItem* P = &s_rqPrev[j];
    if (P->fps_world && P->fps_key) prevcr.emplace(P->fps_key, P->fps_cr); }

  // ---- BACKGROUND / SEA-ATLAS layer 60fps translation (issue #25) -----------------------------------
  // The screen-space scrolling backdrop (RQ_BACKGROUND tiles) carries fps_world=0, so without help it
  // SNAPPED to camera-B while the world meshes/billboards interpolated -> the backdrop juddered. The whole
  // backdrop layer translates RIGIDLY in screen space as the camera scrolls (a tile only moves; its atlas
  // cell = texpage/clut/mode/UV is invariant). So we recover ONE per-frame layer translation by matching
  // each cur bg tile to the prev frame's by that position-INDEPENDENT atlas fingerprint, then take the
  // MEDIAN per-axis displacement (median, not mean — robust to parallax outliers and the odd mismatch, and
  // it can NOT explode the way the retired general per-prim matcher did). The in-between then shifts every
  // bg tile by HALF that median (the t=0.5 midpoint), panning the backdrop with the world. PC-OWNED: no
  // guest read, derived purely from the engine's own queued screen geometry.
  auto bg_fp = [](const RqItem* it) -> uint64_t {        // atlas-cell fingerprint (stable across frames)
    return (uint64_t)(uint32_t)it->tp_x        ^ ((uint64_t)(uint32_t)it->tp_y   << 12)
         ^ ((uint64_t)(uint32_t)it->clut_x << 20) ^ ((uint64_t)(uint32_t)it->clut_y << 28)
         ^ ((uint64_t)(uint32_t)it->mode  << 36) ^ ((uint64_t)(uint32_t)(it->us[0] & 0xFF) << 40)
         ^ ((uint64_t)(uint32_t)(it->vs[0] & 0xFF) << 48); };
  std::unordered_map<uint64_t, const RqItem*> prevbg;
  prevbg.reserve((size_t)s_nPrev + 16);
  for (int j = 0; j < s_nPrev; j++) { const RqItem* P = &s_rqPrev[j];
    if (P->layer == RQ_BACKGROUND) prevbg.emplace(bg_fp(P), P); }   // first occurrence per cell wins
  // collect per-tile displacements, then median each axis
  static int* s_bgdx = 0; static int* s_bgdy = 0;
  if (!s_bgdx) { s_bgdx = new int[FPS60_RQ_MAX]; s_bgdy = new int[FPS60_RQ_MAX]; }
  int nbg = 0;
  for (int i = 0; i < s_nCur && nbg < FPS60_RQ_MAX; i++) {
    const RqItem* C = &s_rqCur[i]; if (C->layer != RQ_BACKGROUND) continue;
    auto it = prevbg.find(bg_fp(C)); if (it == prevbg.end()) continue;
    s_bgdx[nbg] = C->xs[0] - it->second->xs[0];
    s_bgdy[nbg] = C->ys[0] - it->second->ys[0]; nbg++;
  }
  int bg_have = 0, bg_hdx = 0, bg_hdy = 0;                // HALF-median bg translation (the midpoint shift)
  if (nbg >= 4) {                                         // need a few matches for a meaningful median
    std::sort(s_bgdx, s_bgdx + nbg); std::sort(s_bgdy, s_bgdy + nbg);
    int mdx = s_bgdx[nbg / 2], mdy = s_bgdy[nbg / 2];     // median per axis (robust)
    // Snap (don't smear) on a teleport/area-cut: a huge median = the whole backdrop jumped, not a pan.
    if (abs(mdx) <= 128 && abs(mdy) <= 128) {
      bg_have = 1;
      // HALF of the B->cur motion = how far the in-between (t=0.5) sits BEHIND the real frame; shift cur back.
      bg_hdx = -(mdx / 2); bg_hdy = -(mdy / 2);
    }
  }

  long moved = 0, snapped = 0, bgmoved = 0;
  for (int i = 0; i < s_nCur; i++) {
    const RqItem* C = &s_rqCur[i];
    s_rqLerp[i] = *C;                                     // default: SNAP (real frame-B position)
    // BACKGROUND layer (#25): shift the whole backdrop by the half-median camera scroll so it pans WITH the
    // world at the midpoint instead of snapping. Uniform integer screen translate (+ the sub-pixel copy).
    if (C->layer == RQ_BACKGROUND) {
      if (bg_have && (bg_hdx || bg_hdy)) {
        for (int k = 0; k < C->nv; k++) {
          s_rqLerp[i].xs[k] = C->xs[k] + bg_hdx; s_rqLerp[i].ys[k] = C->ys[k] + bg_hdy;
          s_rqLerp[i].xsf[k] = C->xsf[k] + (float)bg_hdx; s_rqLerp[i].ysf[k] = C->ysf[k] + (float)bg_hdy;
        }
        bgmoved++;
      } else snapped++;
      continue;
    }
    if (!C->fps_world || !C->fps_key) { snapped++; continue; }   // 2D/HUD/terrain/unkeyed → snap
    auto it = prevcr.find(C->fps_key);
    if (it == prevcr.end()) { snapped++; continue; }     // actor is new this frame (spawn/teleport) → snap
    uint32_t crM[11]; fps60_compose_mid(it->second, C->fps_cr, crM);
    RqItem tmp = *C;
    // 3D-positioned 2D quad (billboard): translate by the anchor's midpoint screen delta; mesh: per-vertex reproject.
    int worst = C->fps_anchor ? fps60_reproject_anchor(C, crM, &tmp) : fps60_reproject(C, crM, &tmp);
    if (worst > FPS60_VTX_GATE) { snapped++; continue; } // cut/teleport/degenerate → snap (no smear)
    for (int k = 0; k < C->nv; k++) { s_rqLerp[i].xs[k] = tmp.xs[k]; s_rqLerp[i].ys[k] = tmp.ys[k];
                                      s_rqLerp[i].xsf[k] = tmp.xsf[k]; s_rqLerp[i].ysf[k] = tmp.ysf[k];
                                      s_rqLerp[i].depth[k] = tmp.depth[k]; }
    moved++;
  }
  if (s_lerpdbg < 0) s_lerpdbg = cfg_dbg("fps60") ? 1 : 0;
  if (s_lerpdbg) fprintf(stderr, "[fps60] f%ld reproject: prims=%d moved=%ld bgmoved=%ld snapped=%ld actors=%zu "
                         "bg(matches=%d have=%d half=%d,%d)\n",
                         s_fence, s_nCur, moved, bgmoved, snapped, prevcr.size(), nbg, bg_have, bg_hdx, bg_hdy);
  // MECHANICAL GATE (PSXPORT_DEBUG=fps60chk): reproject every world prim at t=1.0 (crM = its OWN captured
  // composed transform, no averaging) — this MUST reproduce the prim's real screen verts. A non-zero error
  // means the capture/recompose/round path is wrong (not a smoothness issue). Pure diagnostic, no output change.
  static int s_chk = -1; if (s_chk < 0) s_chk = cfg_dbg("fps60chk") ? 1 : 0;
  if (s_chk) {
    long n = 0, maxe = 0; double sume = 0;
    for (int i = 0; i < s_nCur; i++) { const RqItem* C = &s_rqCur[i];
      if (!C->fps_world || !C->fps_key) continue;
      RqItem tmp = *C;
      int e = C->fps_anchor ? fps60_reproject_anchor(C, C->fps_cr, &tmp)   // t=1.0: anchor delta = 0
                            : fps60_reproject(C, C->fps_cr, &tmp);
      n++; sume += e; if (e > maxe) maxe = e;
    }
    fprintf(stderr, "[fps60chk] f%ld world=%ld  t=1.0 reproject error: max=%ld avg=%.3f px (0 = exact)\n",
            s_fence, n, maxe, n ? sume / n : 0.0);
  }
  return s_nCur;
}

void gpu_vk_shot(Core* core, const char* path);   // diagnostic: dump the CURRENT s_tex (the just-presented frame)
// PSXPORT_DEBUG=fps60pass — prove the two 60fps presents emit the SAME COMPLETE frame: count the HUD-layer
// items and the shadow-casting prims in a queue set. Both passes must report identical counts (the HUD and
// the shadow are in the queue, so each pass emits them — no per-feature replay/keep_shadow).
static void fps60_pass_stats(const char* tag, long fence, const RqItem* items, int n) {
  static int on = -1; if (on < 0) on = cfg_dbg("fps60pass") ? 1 : 0; if (!on) return;
  long hud = 0, shcast = 0;
  for (int i = 0; i < n; i++) { if (items[i].layer == RQ_HUD) hud++; if (items[i].sh_cast) shcast++; }
  fprintf(stderr, "[fps60pass] f%ld %s: prims=%d HUD=%ld shadow_cast=%ld\n", fence, tag, n, hud, shcast);
}
void Fps60State::fps60_present_vk(Core* core) {
  int nl = (s_have_prev && s_nCur > 0) ? build_lerp() : 0;
  if (nl > 0) {                                           // PASS 1 — the interpolated in-between
    fps60_pass_stats("interp", s_fence, s_rqLerp, nl);
    for (int i = 0; i < nl; i++) gpu_emit_rq_item(core, &s_rqLerp[i]);
    gpu_fps60_present_pass(core);                         // show it + reset the VK batch (no s_frame++)
    // PSXPORT_FPS60_INTERPSHOT=path — one-shot: dump the INTERPOLATED in-between's s_tex (it persists until
    // the real pass overwrites it) so the 60fps in-between (mover at midpoint, shadow/SSAO/2D from the real
    // composite) can be eyeballed in isolation. Pure diagnostic; armed once, then disarmed.
    // PSXPORT_FPS60_INTERPSHOT="path[:fence]" — dump at logic-fence `fence` (default: the first interp frame).
    { static int armed = -1, tfence = -1; static char path[256];
      if (armed < 0) { const char* e = cfg_str("PSXPORT_FPS60_INTERPSHOT");
        armed = (e && *e) ? 1 : 0;
        if (armed) { const char* col = strrchr(e, ':');
          if (col) { tfence = atoi(col + 1); snprintf(path, sizeof path, "%.*s", (int)(col - e), e); }
          else snprintf(path, sizeof path, "%s", e); } }
      if (armed == 1 && (tfence < 0 || s_fence >= tfence)) { gpu_vk_shot(core, path); armed = 2;
        fprintf(stderr, "[fps60] interp-frame shot (f%ld) -> %s\n", s_fence, path); } }
    gpu_pace_subframe(core, 2);
  }
  fps60_pass_stats("real  ", s_fence, s_rqCur, s_nCur);
  for (int i = 0; i < s_nCur; i++) gpu_emit_rq_item(core, &s_rqCur[i]);   // PASS 2 — the real frame
  gpu_present_ex(core, 1);                                // present + per-logic-frame bookkeeping
  gpu_pace_subframe(core, nl > 0 ? 2 : 1);
  if (!s_rqPrev) s_rqPrev = new RqItem[FPS60_RQ_MAX];     // current -> previous for next frame
  if (s_nCur > 0) memcpy(s_rqPrev, s_rqCur, (size_t)s_nCur * sizeof(RqItem));
  s_nPrev = s_nCur; s_have_prev = 1;
  // Billboard registry: it is the CURRENT-frame node→transform map consumed by the OT walk while this frame's
  // queue is built; the prev transform now lives on the prev RqItems (s_rqPrev), so we only reset the cur
  // registry for the next frame's object-render phase (the recorder appends into s_bbCur during that phase).
  s_nBBCur = 0;
}

// ---- Public capture API: thin free-function wrappers over the per-instance Fps60State methods.
// Keep the C-style call sites stable; each forwards to core->game->fps60 (de-globalization, 2026-06-19). ----
void fps60_rtp(Core* core, uint32_t op) { core->game->fps60.rtp(op); }
void fps60_join_poly(Core* core, int px, int py) { core->game->fps60.join_poly(px, py); }
void fps60_frame_commit(Core* core) { core->game->fps60.frame_commit(core); }

void fps60_init(void) {
  // 60fps is toggled in the F1 overlay (persisted to psxport_settings.ini via mods); g_fps60_on is loaded
  // by mods_init BEFORE this runs. NO env gate (user directive): do not read PSXPORT_FPS60 — that would
  // clobber the persisted overlay setting. Just report the loaded state.
  if (g_fps60_on) fprintf(stderr, "[fps60] interpolated 60fps ON (overlay)\n");
}
