// fps60 — TRUE PER-OBJECT interpolated-60fps tier (redesign 2026-07-10, user directive). See fps60.h for
// the architecture. In one sentence: capture each object's WORLD transform + the scene CAMERA host-side
// every logic frame, then render the in-between frame by RE-RUNNING the real native scene render
// (Render::sceneNative + the float projection) with a midpoint-transform provider armed — no screen-space
// reproject, no fingerprint matcher. Billboards (guest OT 2D quads) carry their captured world anchor and
// re-project through the same projection at the interpolated anchor + camera. Host-only; gated g_mods.fps60.
#include "core.h"
#include "game.h"     // Fps60 (per-instance) via core->game->fps60; RenderQueue rq
#include "render.h"   // class Render — sceneNative(); DisplayPassGuard (via render_mode.h)
#include "render_queue.h"
#include "cfg.h"
#include "mods.h"     // Mods (game->mods.fps60)
#include "fs_util.h"  // Fs::ensureParentDirs — no hand-rolled mkdir
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unordered_map>
#include <utility>   // std::swap (pointer-swap double buffer rotation, stage 1)

extern "C" { uint32_t GTE_ReadDR(unsigned); }   // Beetle GTE (mednafen gte.c) — RTP result regs (rate tap)
uint32_t gte_read_ctrl(uint32_t reg);            // GTE control-reg read (projection consts)

// Present primitives (gpu_native.cpp): the real per-frame present + the 60fps in-between pass + the pacer.
void gpu_present_ex(Core* core, int do_blit);
void gpu_fps60_present_pass(Core* core);
void gpu_pace_subframe(Core* core, int n);

#define FPS60_RQ_MAX RQ_MAX

Fps60::~Fps60() { delete[] mRqCur; delete[] mRqPrev; delete[] mRqLerp; }

// ---- logic-rate detector (validated lrate_proto) -----------------------------------------------------
void Fps60::fold(uint32_t v) {
  uint64_t h = mFrameHash;
  for (int i = 0; i < 4; i++) { h ^= (v & 0xFF); h *= 1099511628211ull; v >>= 8; }
  mFrameHash = h; mFrameGeom++;
}
// gte RTP tap (fps60 gate): fold this vertex's projected SXY into the frame fingerprint. RTPS(0x01) writes
// one SXY (DR14); RTPT(0x30) writes three (DR12/13/14). This is the ONLY remaining GTE tap — it feeds the
// rate detector so the tier knows the logic rate (Tomba2 = 30fps → one in-between per frame).
void Fps60::rtp(uint32_t op) {
  if (!game->mods.fps60) return;
  unsigned lo = (op == 0x30) ? 12 : 14;
  for (unsigned r = lo; r <= 14; r++) fold(GTE_ReadDR(r));
}
static void rate_tick(RateDet* d, uint64_t set_hash) {
  if (set_hash == d->last_hash) { d->held++; return; }
  int p = d->held + 1;
  if (p >= 1 && p <= 8) d->votes[p]++;
  int best = 0, bp = 2;
  for (int i = 1; i <= 8; i++) if (d->votes[i] > best) { best = d->votes[i]; bp = i; }
  d->period = bp;
  d->last_hash = set_hash; d->held = 0; d->changes++;
}

// ---- transform capture / midpoint provider -----------------------------------------------------------
void Fps60::beginCapture() {
  if (!game->mods.fps60) return;
  mObjCur.clear();
  mCamCur.valid = false;
  mScrollCur.valid = false;
}

// midpoint of the two captured scene cameras (true /4096 rotation scale) — the in-between camera.
static Fps60::Cam camMidOf(const Fps60::Cam& a, const Fps60::Cam& b, float t) {
  Fps60::Cam m; m.valid = true;
  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) m.R[i][j] = a.R[i][j] + (b.R[i][j] - a.R[i][j]) * t;
                                m.T[i] = a.T[i] + (b.T[i] - a.T[i]) * t; }
  m.ofx = a.ofx + (b.ofx - a.ofx) * t; m.ofy = a.ofy + (b.ofy - a.ofy) * t; m.H = a.H + (b.H - a.H) * t;
  return m;
}

void Fps60::sceneCam(Core* c, float R[3][3], float T[3], float& ofx, float& ofy, float& H) {
  // Read the scene camera from the scratchpad view matrix (CR0-4 halfword packing @ SCR+0xF8; translation
  // @ SCR+0x10C) + the projection constants (CR24 OFX / CR25 OFY 16.16 / CR26 H). R is the RAW int16 rows
  // (undivided — the convention projComposeCore/projComposeCamera consume). This runs UNCONDITIONALLY: it
  // is the camera reader for the whole native projection path, so when fps60 is off it reproduces the exact
  // bytes the old inline reads produced (byte-identical 30fps path).
  const uint32_t SCR = 0x1F800000u;
  uint32_t w0 = c->mem_r32(SCR + 0xF8), w1 = c->mem_r32(SCR + 0xFC), w2 = c->mem_r32(SCR + 0x100),
           w3 = c->mem_r32(SCR + 0x104), w4 = c->mem_r32(SCR + 0x108);
  R[0][0] = (int16_t)w0;         R[0][1] = (int16_t)(w0 >> 16); R[0][2] = (int16_t)w1;
  R[1][0] = (int16_t)(w1 >> 16); R[1][1] = (int16_t)w2;         R[1][2] = (int16_t)(w2 >> 16);
  R[2][0] = (int16_t)w3;         R[2][1] = (int16_t)(w3 >> 16); R[2][2] = (int16_t)w4;
  T[0] = (float)(int32_t)c->mem_r32(SCR + 0x10C);
  T[1] = (float)(int32_t)c->mem_r32(SCR + 0x110);
  T[2] = (float)(int32_t)c->mem_r32(SCR + 0x114);
  ofx = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;
  ofy = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
  H   = (float)(uint16_t)gte_read_ctrl(26);
  if (!game->mods.fps60) return;
  // Capture (true /4096 scale, so billboard world-anchor projection + camera lerp are in a single scale).
  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) mCamCur.R[i][j] = R[i][j] / 4096.0f;
                                mCamCur.T[i] = T[i]; }
  mCamCur.ofx = ofx; mCamCur.ofy = ofy; mCamCur.H = H; mCamCur.valid = true;
  // Mid-present: hand the caller the (prev,cur) t midpoint so terrain/scene-table/objects all pan through
  // the SAME interpolated camera. Convert the /4096 midpoint rotation back to raw int16 units for the caller.
  if (mInterp && mCamPrev.valid) {
    Cam mid = camMidOf(mCamPrev, mCamCur, mT);
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) R[i][j] = mid.R[i][j] * 4096.0f;
                                  T[i] = mid.T[i]; }
    ofx = mid.ofx; ofy = mid.ofy; H = mid.H;
  }
}

void Fps60::objXform(uint32_t cmd, float R[3][3], float T[3]) {
  if (!game->mods.fps60 || !cmd) return;
  ObjX cur;
  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) cur.R[i][j] = R[i][j];
                                cur.T[i] = T[i]; }
  mObjCur[cmd] = cur;
  if (!mInterp) return;
  auto it = mObjPrev.find(cmd);
  if (it == mObjPrev.end()) return;   // new this frame (spawn) → no interp, render at its real pose
  const ObjX& p = it->second;
  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) R[i][j] = p.R[i][j] + (cur.R[i][j] - p.R[i][j]) * mT;
                                T[i] = p.T[i] + (cur.T[i] - p.T[i]) * mT; }
}

void Fps60::bgScroll(int& sx, int& sy) {
  if (!game->mods.fps60) return;
  mScrollCur.x = sx; mScrollCur.y = sy; mScrollCur.valid = true;
  if (mInterp && mScrollPrev.valid) {
    sx = (int)lroundf((float)mScrollPrev.x + (float)(mScrollCur.x - mScrollPrev.x) * mT);
    sy = (int)lroundf((float)mScrollPrev.y + (float)(mScrollCur.y - mScrollPrev.y) * mT);
  }
}

// project a WORLD point through a scene camera (the real float projection, camera-only: view = R·w + T),
// returning float screen coords. R is /4096 true scale; matches native_terrain / EObjXform::project.
static void projWorld(const Fps60::Cam& cam, float wx, float wy, float wz, float& sx, float& sy) {
  float vx = cam.R[0][0]*wx + cam.R[0][1]*wy + cam.R[0][2]*wz + cam.T[0];
  float vy = cam.R[1][0]*wx + cam.R[1][1]*wy + cam.R[1][2]*wz + cam.T[1];
  float vz = cam.R[2][0]*wx + cam.R[2][1]*wy + cam.R[2][2]*wz + cam.T[2];
  float nearp = cam.H * 0.5f; if (nearp < 1.0f) nearp = 1.0f;
  float pz = vz < nearp ? nearp : vz;
  float ph = pz > 0.0f ? cam.H / pz : 0.0f;
  sx = cam.ofx + vx * ph;
  sy = cam.ofy + vy * ph;
}

// Unproject a quad's screen centroid back to a WORLD point, using the node's own world position only to
// pick the view-space DEPTH plane the quad was drawn at (screen-aligned billboard quads share one depth
// with the node that emitted them). This is the exact inverse of projWorld's ph = H/pz screen mapping, so
// projecting the returned world point back through the SAME camera reproduces (cx,cy) to float precision.
// R is /4096 true scale (matches cam's storage convention, see sceneCam) and orthonormal, so its inverse
// is its transpose — no separate inverse-matrix machinery needed.
static void unprojectQuadAnchor(const Fps60::Cam& cam, float nwx, float nwy, float nwz,
                                 float cx, float cy, float out[3]) {
  float vz = cam.R[2][0]*nwx + cam.R[2][1]*nwy + cam.R[2][2]*nwz + cam.T[2];
  float nearp = cam.H * 0.5f; if (nearp < 1.0f) nearp = 1.0f;
  if (vz < nearp) vz = nearp;
  float vx = cam.H > 0.0f ? (cx - cam.ofx) * vz / cam.H : 0.0f;
  float vy = cam.H > 0.0f ? (cy - cam.ofy) * vz / cam.H : 0.0f;
  float dx = vx - cam.T[0], dy = vy - cam.T[1], dz = vz - cam.T[2];
  out[0] = cam.R[0][0]*dx + cam.R[1][0]*dy + cam.R[2][0]*dz;
  out[1] = cam.R[0][1]*dx + cam.R[1][1]*dy + cam.R[2][1]*dz;
  out[2] = cam.R[0][2]*dx + cam.R[1][2]*dy + cam.R[2][2]*dz;
}

// ---- billboard registry ------------------------------------------------------------------------------
// Record a billboard object's packet-pool span [lo,hi) + identity + WORLD position (node+46/50/54, the
// triple billboardCompose1/2 transform through the camera — see perobj_billboard.cpp). Host-only reads.
void Fps60::recordBillboardSpan(Core* c, uint32_t lo, uint32_t hi, uint32_t ident) {
  if (!ident || hi <= lo || mNBbCur >= kBbMax) return;
  BbSpan* e = &mBbCur[mNBbCur++];
  e->lo = lo | 0x80000000u; e->hi = hi | 0x80000000u; e->ident = ident;   // KSEG (matches s_cur_node form)
  e->wx = (float)c->mem_r16s(ident + 46);
  e->wy = (float)c->mem_r16s(ident + 50);
  e->wz = (float)c->mem_r16s(ident + 54);
}
// Per-particle anchor: the caller (billboardEmit) already computed the interpolatable WORLD anchor for THIS
// particle (node world pos + node-rotation × the particle's own 5×offset). Store it keyed by particle addr.
void Fps60::recordBillboardParticle(uint32_t pktLo, uint32_t pktHi, uint32_t ident, float wx, float wy, float wz) {
  if (!ident || pktHi <= pktLo || mNBbPart >= kBbPartMax) return;
  BbSpan* e = &mBbPart[mNBbPart++];
  e->lo = pktLo | 0x80000000u; e->hi = pktHi | 0x80000000u; e->ident = ident;   // KSEG (matches s_cur_node)
  e->wx = wx; e->wy = wy; e->wz = wz;
}
int Fps60::billboardForNode(uint32_t node, uint32_t* identOut, float wpos[3], int* which, int* spanIdxOut) const {
  uint32_t n = node | 0x80000000u;
  // Per-particle anchors win: a gem sprite's OT packet lies INSIDE its manager node's span, so search the
  // finer per-particle table first — otherwise every sprite of a manager resolves to the manager's single
  // node anchor (the rigid-translate bug). Node-level spans remain the fallback for non-particle billboards.
  for (int i = 0; i < mNBbPart; i++)
    if (n >= mBbPart[i].lo && n < mBbPart[i].hi) {
      if (identOut) *identOut = mBbPart[i].ident;
      if (wpos) { wpos[0] = mBbPart[i].wx; wpos[1] = mBbPart[i].wy; wpos[2] = mBbPart[i].wz; }
      if (which) *which = 1;
      return 1;
    }
  for (int i = 0; i < mNBbCur; i++)
    if (n >= mBbCur[i].lo && n < mBbCur[i].hi) {
      if (identOut) *identOut = mBbCur[i].ident;
      if (wpos) { wpos[0] = mBbCur[i].wx; wpos[1] = mBbCur[i].wy; wpos[2] = mBbCur[i].wz; }
      if (which) *which = 2;
      if (spanIdxOut) *spanIdxOut = i;
      return 1;
    }
  if (which) *which = 0;
  return 0;
}
// Stamp the just-queued billboard RqItem (RQ_WORLD/RQ_OM_DEPTH, from the OT walk) with its object's
// identity + world anchor position, so the mid-present can re-project the anchor through the interpolated
// camera and place the sprite there. Called from the OT walk (gpu_native.cpp) with the source OT-node.
void Fps60::stampBillboard(Core* c, uint32_t node) {
  if (!game->mods.fps60) return;
  RenderQueue& q = c->game->rq;
  if (q.consumed || q.n == 0) return;
  RqItem* it = &q.items[q.n - 1];
  if (it->layer != RQ_WORLD || it->order_mode != RQ_OM_DEPTH) return;
  uint32_t ident; float wpos[3]; int which = 0, spanIdx = -1;
  if (!billboardForNode(node, &ident, wpos, &which, &spanIdx)) {
    if (cfg_dbg("bbanchor")) fprintf(stderr, "[bbanchor][stamp] node=%08X MISS (no table hit)\n", node);
    return;
  }
  // #45 fix (node-span teleport): a NODE-SPAN table hit (which==2) means many quads in this span all
  // matched the SAME entry — one static object-granular anchor (node+46/50/54) for potentially many
  // billboard quads scattered across the object (e.g. a tree's 8-51 fruit/leaf quads). Reprojecting that
  // single anchor at the mid-present moves EVERY quad in the span by the same screen delta, so any quad
  // whose real position isn't at the node's anchor teleports toward it — confirmed numerically (fruit
  // quad's real base SXY (250,4) vs the node-anchor's projected SXY (289,155), ~150px off).
  //
  // Per-particle table entries (mBbPart, which==1) already carry a per-quad anchor computed forward from
  // the guest transform (billboardEmit) and are UNCHANGED here — only the node-span fallback is wrong.
  //
  // Fix: for a node-span hit, don't store the node's world pos as the anchor. Instead UNPROJECT this
  // quad's own screen centroid back to world space, using the node's world position only to pick the
  // view-space depth plane (screen-aligned quads in one span share the node's depth). The unprojection is
  // the exact algebraic inverse of projWorld's camera projection (same R//4096, T, ofx/ofy/H convention),
  // so projecting the result back through the SAME camera reproduces this quad's exact base screen
  // position by construction — the mid-present's motion is then pure camera interpolation per quad,
  // not a shared rigid translate.
  float quadCx = 0.0f, quadCy = 0.0f;   // DIAGNOSTIC (bbanchor log below): the quad's own screen centroid —
                                          // the unprojection target when which==2, so the log can show the
                                          // anchor round-trips EXACTLY to its actual target (not vertex 0).
  if (which == 2) {
    int nv = it->nv; if (nv < 1) nv = 1; if (nv > 4) nv = 4;
    for (int k = 0; k < nv; k++) {
      quadCx += it->has_xyf ? it->xsf[k] : (float)it->xs[k];
      quadCy += it->has_xyf ? it->ysf[k] : (float)it->ys[k];
    }
    quadCx /= (float)nv; quadCy /= (float)nv;
    if (mCamCur.valid) {
      float quadWpos[3];
      unprojectQuadAnchor(mCamCur, wpos[0], wpos[1], wpos[2], quadCx, quadCy, quadWpos);
      wpos[0] = quadWpos[0]; wpos[1] = quadWpos[1]; wpos[2] = quadWpos[2];
    }
    // else: mCamCur not captured this frame (shouldn't happen — sceneNative runs before the OT walk in
    // drawOTag) — fall back to the node anchor rather than unproject through a garbage camera.
  }
  it->fps_anchor = 1; it->fps_key = ident;
  it->fps_wpos[0] = wpos[0]; it->fps_wpos[1] = wpos[1]; it->fps_wpos[2] = wpos[2];
  if (cfg_dbg("bbanchor")) {
    fprintf(stderr,
      "[bbanchor][stamp] node=%08X which=%s ident=%08X anchor=(%.2f,%.2f,%.2f)\n",
      node, which == 1 ? "PARTICLE" : (which == 2 ? "NODE-SPAN" : "MISS"), ident,
      wpos[0], wpos[1], wpos[2]);
    if (which == 2 && spanIdx >= 0 && spanIdx < kBbMax) {
      int stampCount = ++mBbCurStamps[spanIdx];
      // Compare the NEW per-quad anchor's reprojection against (a) THIS quad's ACTUAL base-frame vertex-0
      // screen pos (quadRealSXY0 — kept for continuity with the pre-fix diagnosis) and (b) its own
      // unprojection TARGET, the quad's screen centroid (quadCentroidSXY/centroidDeltaPx) — the fix
      // unprojects the centroid, not vertex 0, so (b) is the round-trip that must be ~0 by construction;
      // the vertex-0 delta is expected to be nonzero (the corner-to-centroid geometric offset, up to
      // ~half the quad's screen size), NOT anchor error.
      float ax0 = it->has_xyf ? it->xsf[0] : (float)it->xs[0];
      float ay0 = it->has_xyf ? it->ysf[0] : (float)it->ys[0];
      float psx, psy;
      if (mCamCur.valid) projWorld(mCamCur, wpos[0], wpos[1], wpos[2], psx, psy); else { psx = psy = -1.0f; }
      fprintf(stderr,
        "[bbanchor][stamp]   span[%d] lo=%08X hi=%08X stampsThisFrame=%d nodeWorldPos(node+46/50/54)=(%.2f,%.2f,%.2f) "
        "anchorProjSXY=(%.1f,%.1f) quadRealSXY0=(%.1f,%.1f) deltaPx=(%.1f,%.1f) quadCentroidSXY=(%.1f,%.1f) "
        "centroidDeltaPx=(%.2f,%.2f)\n",
        spanIdx, mBbCur[spanIdx].lo, mBbCur[spanIdx].hi, stampCount,
        (double)(int16_t)c->mem_r16(ident + 46), (double)(int16_t)c->mem_r16(ident + 50), (double)(int16_t)c->mem_r16(ident + 54),
        psx, psy, ax0, ay0, psx - ax0, psy - ay0, quadCx, quadCy, psx - quadCx, psy - quadCy);
    }
  }
  // objid overlay recovery: a billboard rasterizes after the render walk's node scope ended, so it was
  // queued with dbg_node=0 — recover it from the span-matched entity node (KSEG).
  if (ident >= 0x80000000u && ident < 0x80200000u) it->dbg_node = ident;
}

// ---- STAGE 2: prim matching + lerp (docs/fps60-rework.md "Match+lerp stage") ---------------------------
// Shape fingerprint: the fields that identify "the same kind of prim" — layer/order_mode/nv (what it is
// and how it's ordered/depth-tested), the resolved texture material (mode/tp_x/tp_y/clut_x/clut_y/
// tw_mx/tw_my/tw_ox/tw_oy — "texture page/clut/texwin fields"), raw/semi/tp_blend (the "op-ish identity" —
// untextured-vs-textured, blended-vs-opaque, and which blend mode), and has_xyf (sub-pixel float path vs
// integer-snap path — drawn differently by emitItem, so a mismatch here is a different kind of prim too).
// A provenance match (same dbg_node + same emission index) whose fingerprint differs is an animation-frame
// swap (e.g. a texture-atlas cell flip) — draw at the nearest real frame, don't lerp across textures.
static bool fpEqual(const RqItem& a, const RqItem& b) {
  return a.layer == b.layer && a.order_mode == b.order_mode && a.nv == b.nv && a.raw == b.raw &&
         a.semi == b.semi && a.tp_blend == b.tp_blend && a.has_xyf == b.has_xyf &&
         a.mode == b.mode && a.tp_x == b.tp_x && a.tp_y == b.tp_y &&
         a.clut_x == b.clut_x && a.clut_y == b.clut_y &&
         a.tw_mx == b.tw_mx && a.tw_my == b.tw_my && a.tw_ox == b.tw_ox && a.tw_oy == b.tw_oy;
}
// Colors/UVs are NOT lerped (the fingerprint match guarantees they're the "same" prim) — but a prim whose
// per-vertex color DOES differ frame-to-frame despite matching (e.g. a fade multiplier baked into rs/gs/bs)
// would lerp-freeze at the wrong tint if drawn with either endpoint's color. Compare and demote to
// unmatched on any difference — draws at its own real frame's color instead of a wrong blend.
static bool colorsEqual(const RqItem& a, const RqItem& b) {
  int nv = a.nv ? a.nv : 4;
  for (int k = 0; k < nv; k++)
    if (a.rs[k] != b.rs[k] || a.gs[k] != b.gs[k] || a.bs[k] != b.bs[k]) return false;
  return true;
}
// FNV-1a fold of the fingerprint fields — the key strength-3 (dbg_node==0) grouping key.
static uint64_t fpHash(const RqItem& a) {
  uint64_t h = 1469598103934665603ull;
  auto fold = [&](uint32_t v) { h ^= v; h *= 1099511628211ull; };
  fold(a.layer); fold(a.order_mode); fold(a.nv); fold(a.raw); fold(a.semi); fold(a.tp_blend); fold(a.has_xyf);
  fold((uint32_t)a.mode); fold((uint32_t)a.tp_x); fold((uint32_t)a.tp_y);
  fold((uint32_t)a.clut_x); fold((uint32_t)a.clut_y);
  fold((uint32_t)a.tw_mx); fold((uint32_t)a.tw_my); fold((uint32_t)a.tw_ox); fold((uint32_t)a.tw_oy);
  return h;
}
// Lerp a matched pair at t: screen-space vertex XY (float — xsf/ysf when has_xyf, else xs/ys promoted to
// float) and per-vertex depth. Everything else (material, UVs, colors) is validated equal by fpEqual/
// colorsEqual above and copied from b. Shadow-cast verts stay b's un-interpolated view-space verts (the
// shadow map is never interpolated — render_queue.h sh_cast contract). Base on b (the newer real frame) so
// residual fields (fps_scene/fps_anchor/fps_key, now unused by stage-2 matching, retired in stage 3) read
// as this frame's, not stale data from two frames back.
static RqItem lerpItem(const RqItem& a, const RqItem& b, float t) {
  RqItem it = b;
  int nv = a.nv ? a.nv : 4;
  for (int k = 0; k < nv; k++) {
    float axf = a.has_xyf ? a.xsf[k] : (float)a.xs[k];
    float ayf = a.has_xyf ? a.ysf[k] : (float)a.ys[k];
    float bxf = b.has_xyf ? b.xsf[k] : (float)b.xs[k];
    float byf = b.has_xyf ? b.ysf[k] : (float)b.ys[k];
    it.xsf[k] = axf + (bxf - axf) * t;
    it.ysf[k] = ayf + (byf - ayf) * t;
    it.xs[k]  = (int)lroundf(it.xsf[k]);
    it.ys[k]  = (int)lroundf(it.ysf[k]);
    it.depth[k] = a.depth[k] + (b.depth[k] - a.depth[k]) * t;
  }
  it.has_xyf = 1;   // promote every matched prim (even 2D/HUD ones) to the sub-pixel path — t=0.5 rarely
                     // lands on an integer, so slot A always carries the lerped float position.
  it.seq = a.seq;    // preserve Q[N-1]'s paint-order tiebreak — slot A keeps A's submission order
  it.dbg_node = a.dbg_node;
  return it;
}

// Per-frame node-emission-index: walk items in their captured (already paint-ordered) sequence, counting
// per dbg_node — objects emit their prims in stable order frame-to-frame (submit.cpp's per-object render
// walk), so (dbg_node, running count) is a stable identity while the object lives. dbg_node==0 (no
// provenance: terrain/static/background prims, or a billboard the OT walk couldn't stamp) gets the "no
// node" sentinel — those are matched separately, by fingerprint + order-of-appearance (key strength 3).
static constexpr uint32_t kNoNode = 0xFFFFFFFFu;
void Fps60::buildProvenanceIdx(const RqItem* items, int n, std::vector<uint32_t>& out) {
  out.resize((size_t)n);
  mNodeIdxScratch.clear();
  for (int i = 0; i < n; i++) {
    uint32_t node = items[i].dbg_node;
    if (!node) { out[i] = kNoNode; continue; }
    out[i] = mNodeIdxScratch[node]++;
  }
}

// Match Q[N-1] (mRqPrev) against Q[N] (mRqCur) and record, per Q[N-1] item, which Q[N] item (if any) it
// pairs with (mMatchOfA). present_vk walks Q[N-1] in its own paint order and either lerps the matched pair
// or draws the Q[N-1] item as-is — so slot A's draw order is exactly Q[N-1]'s order (matches stage 1's
// "verbatim replay" ordering for anything that doesn't get a match). An unmatched Q[N] item simply has no
// Q[N-1] item pointing at it, so it never appears in slot A — it pops in at slot B, same 30fps cadence as
// the pre-fps60 path (per docs/fps60-rework.md "Prim matching").
void Fps60::matchAndLerp(Core*) {
  const RqItem* A = mRqPrev; int nA = mNPrev;   // Q[N-1]
  const RqItem* B = mRqCur;  int nB = mNCur;    // Q[N]
  if (!mRqLerp) mRqLerp = new RqItem[FPS60_RQ_MAX];
  mMatchedThisFrame = 0; mUnmatchedThisFrame = 0;

  buildProvenanceIdx(A, nA, mIdxPrevBuf);
  buildProvenanceIdx(B, nB, mIdxCurBuf);

  // Q[N] provenance lookup: (dbg_node, emission-index) -> item index. dbg_node==0 items are excluded here
  // (handled by the fingerprint/order-of-appearance pass below).
  mMatchMap.clear();
  for (int j = 0; j < nB; j++)
    if (mIdxCurBuf[j] != kNoNode) mMatchMap[((uint64_t)B[j].dbg_node << 32) | mIdxCurBuf[j]] = j;

  // dbg_node==0 groups, in each frame's own emission order, keyed by shape fingerprint.
  mZeroGroupsPrev.clear(); mZeroGroupsCur.clear();
  for (int i = 0; i < nA; i++) if (mIdxPrevBuf[i] == kNoNode) mZeroGroupsPrev[fpHash(A[i])].push_back(i);
  for (int j = 0; j < nB; j++) if (mIdxCurBuf[j]  == kNoNode) mZeroGroupsCur[fpHash(B[j])].push_back(j);

  mMatchOfA.assign((size_t)nA, -1);
  mUsedB.assign((size_t)nB, 0);

  // Pass 1 (key strength 1+2): provenance-keyed prims, validated by fingerprint + color on every hit.
  for (int i = 0; i < nA; i++) {
    if (mIdxPrevBuf[i] == kNoNode) continue;
    auto it = mMatchMap.find(((uint64_t)A[i].dbg_node << 32) | mIdxPrevBuf[i]);
    if (it == mMatchMap.end()) continue;                          // unmatched — draws as-is
    int j = it->second;
    if (mUsedB[j] || !fpEqual(A[i], B[j]) || !colorsEqual(A[i], B[j])) continue;   // demoted — draws as-is
    mMatchOfA[i] = j; mUsedB[j] = 1;
  }

  // Pass 2 (key strength 3): dbg_node==0 prims, matched by (fingerprint, order-of-appearance), ONLY when
  // the two frames agree on how many prims share that fingerprint — otherwise the whole group is left
  // unmatched (an ambiguous count means order-of-appearance isn't a trustworthy identity this frame).
  for (auto& kv : mZeroGroupsPrev) {
    auto cit = mZeroGroupsCur.find(kv.first);
    const std::vector<int>& prevIdx = kv.second;
    if (cit == mZeroGroupsCur.end() || cit->second.size() != prevIdx.size()) continue;
    const std::vector<int>& curIdx = cit->second;
    for (size_t k = 0; k < prevIdx.size(); k++) {
      int i = prevIdx[k], j = curIdx[k];
      if (mUsedB[j] || !fpEqual(A[i], B[j]) || !colorsEqual(A[i], B[j])) continue;
      mMatchOfA[i] = j; mUsedB[j] = 1;
    }
  }

  // Emit: walk Q[N-1] in its captured paint order. Matched -> lerp(A,B,t); unmatched -> A as-is.
  mNLerp = 0;
  for (int i = 0; i < nA && mNLerp < FPS60_RQ_MAX; i++) {
    if (mMatchOfA[i] >= 0) { mRqLerp[mNLerp++] = lerpItem(A[i], B[mMatchOfA[i]], mT); mMatchedThisFrame++; }
    else                   { mRqLerp[mNLerp++] = A[i];                              mUnmatchedThisFrame++; }
  }
  mMatchedTotal += mMatchedThisFrame; mUnmatchedTotal += mUnmatchedThisFrame;
  // Match-rate telemetry: once/sec (logic runs ~30fps -> every 30 fences) under PSXPORT_DEBUG=fps60.
  if (mDbg && (mFence % 30) == 0) {
    long tot = mMatchedTotal + mUnmatchedTotal;
    fprintf(stderr, "[fps60] f%ld match-rate: this-frame matched=%ld/%d (%.0f%%) unmatched=%ld | "
                     "running matched=%ld/%ld (%.0f%%)\n",
            mFence, mMatchedThisFrame, nA, nA ? 100.0 * mMatchedThisFrame / nA : 0.0, mUnmatchedThisFrame,
            mMatchedTotal, tot, tot ? 100.0 * mMatchedTotal / tot : 0.0);
  }
}

// ---- per-present frame dump (debug channel `fps60dump`) ----------------------------------------------
// Writes what THIS present pass just put in s_vram_tex, exactly like REPL `shot` — same VRAM-readback
// writer (gpu_gpu_shot/gpu_native_shot), no new pixel path. Must run right after the present call that
// filled the target (present_vk's PASS 1 for interp, PASS 2 for real) so the readback sees that pass's
// content, not the next one's.
void Fps60::dumpPresent(Core* core, bool interp) {
  if (!cfg_dbg("fps60dump")) { mDumpSeq = 0; return; }   // channel off: idle, and reset the cap for next arm
  if (mDumpSeq >= kDumpMax) {
    if (mDumpSeq == kDumpMax) { fprintf(stderr, "[fps60dump] cap (%d files) reached — stop capturing\n", kDumpMax); mDumpSeq++; }
    return;
  }
  char path[192];
  snprintf(path, sizeof path, "scratch/framedump/f%06ld_%04d_%s.png", mFence, mDumpSeq, interp ? "interp" : "real");
  if (!Fs::ensureParentDirs(path)) return;
  int gpu_gpu_enabled(void); void gpu_gpu_shot(Core*, const char*); void gpu_native_shot(Core*, const char*);
  if (gpu_gpu_enabled()) gpu_gpu_shot(core, path); else gpu_native_shot(core, path);
  mDumpSeq++;
}

// ---- per-logic-frame fence + present -----------------------------------------------------------------
// rq_capture always writes the newly-flushed queue into `mRqCur` — the buffer that, after the PREVIOUS
// present_vk's end-of-frame pointer swap, holds the now-stale two-frames-ago content (never needed again,
// safe to overwrite). `mRqPrev` is left untouched here — it still holds last frame's real queue, which
// present_vk's slot A is about to replay verbatim. See the swap at the end of present_vk.
void Fps60::rq_capture(const RqItem* items, int n) {
  if (!mRqCur)  mRqCur  = new RqItem[FPS60_RQ_MAX];
  if (!mRqPrev) mRqPrev = new RqItem[FPS60_RQ_MAX];
  if (n > FPS60_RQ_MAX) n = FPS60_RQ_MAX;
  if (n > 0) memcpy(mRqCur, items, (size_t)n * sizeof(RqItem));
  mNCur = n;
}

void Fps60::frame_commit(Core* core) {
  if (!game->mods.fps60) return;
  uint64_t set_hash = (mFrameGeom > 0) ? mFrameHash : 0xFFFFFFFFFFFFFFFFull;
  rate_tick(&mRd, set_hash);
  mFence++;
  if (!core->game->diff_mode) present_vk(core);
  mFrameHash = 1469598103934665603ull;
  mFrameGeom = 0;
}

// STAGE 2 (docs/fps60-rework.md "Match+lerp stage"): slot A draws lerp(Q[N-1], Q[N], t=0.5) — matched
// prim pairs (see matchAndLerp) interpolate their screen-space verts + depth; prims with no confident
// cross-frame match draw at Q[N-1] as-is (stage 1's "verbatim replay" behavior, now the fallback for the
// unmatched case rather than the whole frame). No guest reads at present time, no second render path — an
// interpolated frame is built ENTIRELY from the two real queues' own captured data, through the exact same
// per-item draw call (`q.emitItem`) slot B uses for Q[N]; it cannot show a game state neither real frame
// had. The anchor/mid-present-rebuild machinery above (sceneCam/objXform/bgScroll midpoint providers,
// billboard stamping, projWorld/unprojectQuadAnchor) is intentionally left in place per the design doc's
// staging — it isn't consulted from this path; stage 3 deletes what's unused.
void Fps60::present_vk(Core* core) {
  Core* c = core;
  RenderQueue& q = c->game->rq;
  if (mDbg < 0) mDbg = cfg_dbg("fps60") ? 1 : 0;

  // ---- PASS 1 (slot A): lerp(Q[N-1], Q[N]) by matched prim, Q[N-1] as-is otherwise -----------------------
  // First frame after enabling fps60 (no Q[N-1] captured yet, mHavePrev==0): degenerate to replaying THIS
  // frame's own queue (Q[N] twice) rather than matching against an empty/garbage buffer — 30fps content at
  // 60Hz pacing for exactly one frame, per the design doc's stage-1 first-frame case (still applies).
  const RqItem* slotA; int nSlotA;
  if (mHavePrev) { matchAndLerp(c); slotA = mRqLerp; nSlotA = mNLerp; }
  else           { slotA = mRqCur;  nSlotA = mNCur; }
  for (int i = 0; i < nSlotA; i++) q.emitItem(c, &slotA[i]);
  gpu_fps60_present_pass(c);
  dumpPresent(c, /*interp=*/true);
  if (mDbg) fprintf(stderr, "[fps60] f%ld slotA: replay prev=%s n=%d\n",
                    mFence, mHavePrev ? "Q[N-1]" : "Q[N] (first frame)", nSlotA);
  gpu_pace_subframe(c, 2);

  // ---- PASS 2 (slot B): the real frame (the captured queue, exactly as drawOTag built it) ---------------
  for (int i = 0; i < mNCur; i++) q.emitItem(c, &mRqCur[i]);
  gpu_present_ex(c, 1);
  dumpPresent(c, /*interp=*/false);
  gpu_pace_subframe(c, 2);

  // ---- rotate captures: POINTER SWAP, not memcpy (avoids the double-copy churn of the old design) -------
  // mRqCur (just-drawn Q[N]) becomes next frame's mRqPrev; the buffer mRqPrev used to point at (now-stale,
  // two-frames-old content that's never read again) becomes rq_capture's next overwrite target.
  std::swap(mRqCur, mRqPrev);
  std::swap(mNCur, mNPrev);
  mHavePrev = 1;
  // (mBbCur is reset at the top of Engine::frameUpdate — the frame boundary before fieldFrame records.)
}

