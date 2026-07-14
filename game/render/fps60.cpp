// fps60 — renderer-internal, one-frame-behind interpolated-60fps tier (docs/fps60-rework.md, redesign
// 2026-07-14). See fps60.h for the architecture. In one sentence: the renderer double-buffers the last two
// real frames' resolved render-queue prims and presents one frame behind — slot A draws a provenance-
// matched vertex lerp of Q[N-1]/Q[N] (matchAndLerp), slot B draws Q[N] verbatim, both through the SAME
// `RenderQueue::emitItem` draw path. No guest reads at present time, no scene re-run, no anchors. Host-only;
// gated g_mods.fps60.
#include "core.h"
#include "game.h"     // Fps60 (per-instance) via core->game->fps60; RenderQueue rq
#include "render.h"   // class Render — sceneNative()/terrainRenderAll(); DisplayPassGuard (render_mode.h)
#include "render_queue.h"
#include "proj_params.h"   // ProjParams::Snapshot — save/restore around tier1Render's re-render
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

Fps60::~Fps60() { delete[] mRqCur; delete[] mRqPrev; delete[] mRqLerp; delete mSink; }

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

// ---- shared camera reader -------------------------------------------------------------------------------
void Fps60::sceneCam(Core* c, float R[3][3], float T[3], float& ofx, float& ofy, float& H) {
  // TIER 1 override (fps60.h "Object-tier attempt 2026-07-14"): while present_vk's tier1Render() is
  // re-invoking terrainRenderAll() at the interp present, hand back the LERPED camera instead of a guest
  // read — no guest reads at present time, matching the fps60 present-time invariant.
  if (mCamOverrideOn) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) R[i][j] = mCamOverride.R[i][j]; T[i] = mCamOverride.T[i]; }
    ofx = mCamOverride.ofx; ofy = mCamOverride.ofy; H = mCamOverride.H;
    return;
  }
  // Read the scene camera from the scratchpad view matrix (CR0-4 halfword packing @ SCR+0xF8; translation
  // @ SCR+0x10C) + the projection constants (CR24 OFX / CR25 OFY 16.16 / CR26 H). R is the RAW int16 rows
  // (undivided — the convention projComposeCore/projComposeCamera consume). This is the ONE camera reader
  // for the whole native projection path (projComposeCore/projComposeCamera/native_terrain), unconditional
  // — not gated on fps60 (fps60 no longer reprojects at present time; it lerps already-resolved queue prims).
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
  // TIER 1 capture: this is a REAL-frame call (mCamOverrideOn is false) — mirror it into mCamCur, the slot
  // that present_vk's end-of-frame swap rotates in lockstep with mRqCur/mRqPrev. Every sceneCam() call this
  // logic frame reads the same unchanged guest camera, so overwriting on every call is idempotent.
  if (game->mods.fps60) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) mCamCur.R[i][j] = R[i][j]; mCamCur.T[i] = T[i]; }
    mCamCur.ofx = ofx; mCamCur.ofy = ofy; mCamCur.H = H;
  }
}

// ---- TIER 1 BACKDROP: game-logic-scroll layer-transform lerp (docs/fps60-rework.md) --------------------
void Fps60::bgScroll(Core* c, uint32_t t4, int& scrollX, int& scrollY) {
  if (mBgOverrideOn) { scrollX = mBgOverride.scrollX; scrollY = mBgOverride.scrollY; return; }
  scrollX = c->mem_r16s(t4 + 0x28u);
  scrollY = c->mem_r16s(t4 + 0x2au);
  // TIER 1 capture: this is a REAL-frame call — mirror it into mBgCur, rotated in lockstep with mRqCur/
  // mRqPrev / mCamCur/mCamPrev by present_vk's end-of-frame swap.
  if (game->mods.fps60) { mBgCur.scrollX = scrollX; mBgCur.scrollY = scrollY; }
}

// Shortest-signed-path lerp for a value the guest wraps into [0, mod) every tick (ParallaxBg::step's
// wrapMod). A naive `prev + (cur-prev)*t` sweeps the LONG way around a wrap boundary — e.g. prev=254,
// cur=2, mod=256: naive t=0.5 gives 128 (all the way across the map); the actual motion was 254->255->0->
// ->1->2, whose midpoint is 0. Exact at t=0/t=1 by construction (integer diff, lroundf(diff*0)=0 and
// lroundf(diff*1)=diff are both exact for integer diff).
static int wrapLerp(int prev, int cur, int mod, float t) {
  if (mod <= 0) return prev + (int)lroundf((float)(cur - prev) * t);
  int diff = cur - prev;
  if (diff >  mod / 2) diff -= mod;
  if (diff < -mod / 2) diff += mod;
  int v = prev + (int)lroundf((float)diff * t);
  v %= mod; if (v < 0) v += mod;
  return v;
}

// ---- TIER 1: camera-lerp native world (terrain) re-render (docs/fps60-rework.md "Object-tier attempt") -
// Re-runs Render::terrainRenderAll() — the SAME enumeration+draw sequence the real per-logic-frame walk
// uses (submit.cpp/render_walk.cpp) — under lerp(mCamPrev, mCamCur, t) instead of a guest camera read
// (sceneCam's mCamOverrideOn branch), with its drawWorldQuad output redirected into the isolated `mSink`
// (Game::rqRedirect; native_terrain.cpp checks it) instead of the live `game->rq`.
//
// INVARIANT (load-bearing, see fps60.h): present_vk runs from Engine::frameUpdate, which native_boot.cpp
// calls BEFORE this iteration's pcSched.step() and drawOTag — so no logic tick for "this" iteration has
// run yet, and terrainRenderAll() re-reads the exact guest state the real terrain call already read this
// interval. Host-computed matrices only (the lerped camera above); DisplayPassGuard aborts on any guest
// write, exactly like the real display pass. Per-Core shared render state incidentally touched by the
// re-render (ProjParams' published camview + H/OFX/OFY) is snapshotted and restored so nothing else ever
// observes the lerped camera.
void Fps60::tier1Render(Core* core, float t) {
  Core* c = core;
  if (!mSink) mSink = new RenderQueue();
  mSink->reset();

  Fps60Cam lerp;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) lerp.R[i][j] = mCamPrev.R[i][j] + (mCamCur.R[i][j] - mCamPrev.R[i][j]) * t;
    lerp.T[i] = mCamPrev.T[i] + (mCamCur.T[i] - mCamPrev.T[i]) * t;
  }
  lerp.ofx = mCamPrev.ofx + (mCamCur.ofx - mCamPrev.ofx) * t;
  lerp.ofy = mCamPrev.ofy + (mCamCur.ofy - mCamPrev.ofy) * t;
  lerp.H   = mCamPrev.H   + (mCamCur.H   - mCamPrev.H)   * t;
  mCamOverride = lerp;
  if (cfg_dbg("terrpc"))
    fprintf(stderr, "[tier1dbg] t=%.3f cur.T=(%.1f,%.1f,%.1f) prev.T=(%.1f,%.1f,%.1f) lerp.T=(%.1f,%.1f,%.1f) "
                     "cur.H=%.1f prev.H=%.1f lerp.H=%.1f ofx=%.1f ofy=%.1f\n",
            t, mCamCur.T[0], mCamCur.T[1], mCamCur.T[2], mCamPrev.T[0], mCamPrev.T[1], mCamPrev.T[2],
            lerp.T[0], lerp.T[1], lerp.T[2], mCamCur.H, mCamPrev.H, lerp.H, lerp.ofx, lerp.ofy);

  ProjParams::Snapshot projSaved = c->mRender->projParams.snapshot();
  RenderQueue* prevRedirect = c->game->rqRedirect;
  c->game->rqRedirect = mSink;
  mCamOverrideOn = true;
  {
    DisplayPassGuard displayPass(c->mRender->mode);   // FAIL-FAST: abort on any guest write, real-path discipline
    c->mRender->terrainRenderAll();
    // SCENE TABLE (grass/terrain props, kSceneTableDbgNode): camera-only, same as terrain — fieldEntityRender
    // composes ONLY the scene camera (projComposeCamera -> sceneCam, honors mCamOverrideOn above), never a
    // per-object transform, and its geometry (SCENE_ENT_TABLE records) is static per-area data, not per-frame
    // mutable state — verified by reading submitPolyGt3Native/Gt4Native (submit.cpp): the only per-frame
    // guest reads left in that path were the record array itself (unchanging while the object lives) and the
    // two bugs fixed alongside this change (a raw CR26 H read bypassing the camera-lerp override, and a
    // direct `game->rq` write bypassing rqRedirect) — both now route through the same choke terrain uses.
    // Gated the same way the real call is (render_walk.cpp sceneNative): mSceneTableTrusted is computed once
    // per real frame and unchanged since (present-time invariant — no tick has run since).
    if (c->mRender->mSceneTableTrusted) c->mRender->fieldEntityRender(0x800F2418u);

    // BACKDROP (game-logic scroll, LAYER-TRANSFORM lerp — not camera-projected, so NOT part of the camera
    // override above): mirrors sceneNative's own gate (render_walk.cpp) exactly — mBackdropTrusted (the
    // same trust latch the real call uses, computed once per real frame and unchanged since, per the
    // present-time invariant) && the field-drawer-selector byte == 0 && bgstate == 0 (only the tilemap
    // drawer, state 0, is natively owned). The wrap moduli (t4+0x30/+0x32) are static per-area config
    // (ParallaxBg INIT), safe to re-read directly here — same as W/H/tilemap-ptr/tpage/clutbase, which
    // backdropRender() itself re-reads directly below (unchanged guest state this interval).
    if (c->mRender->mBackdropTrusted && c->mem_r8(0x800bf873u) == 0 && c->mem_r8(0x800bf870u) == 0) {
      int modX = c->mem_r16(0x800ed018u + 0x30u), modY = c->mem_r16(0x800ed018u + 0x32u);
      mBgOverride.scrollX = wrapLerp(mBgPrev.scrollX, mBgCur.scrollX, modX, t);
      mBgOverride.scrollY = wrapLerp(mBgPrev.scrollY, mBgCur.scrollY, modY, t);
      mBgOverrideOn = true;
      c->mRender->backdropRender(0x800ed018u);
      mBgOverrideOn = false;
    }
  }
  mCamOverrideOn = false;
  c->game->rqRedirect = prevRedirect;
  c->mRender->projParams.restore(projSaved);

  mSink->sortQueue();
  // Split mSink's telemetry by producer: RQ_BACKGROUND (backdrop) vs RQ_WORLD (terrain+scene-table) — the
  // two producers sharing the sink, distinguished the same way the debug print already reported them.
  mBackdropPrimsThisFrame = 0;
  for (int i = 0; i < mSink->n; i++) if (mSink->items[i].layer == RQ_BACKGROUND) mBackdropPrimsThisFrame++;
  mTier1PrimsThisFrame = mSink->n - mBackdropPrimsThisFrame;
  // DIAG (debug channel "tier1sc"): the aggregate screen bbox tier1Render actually re-rendered this
  // present, split by producer — used once to data-derive the scene-table's on-screen footprint for the
  // exactness gate (docs/fps60-rework.md), not load-bearing.
  if (cfg_dbg("tier1sc")) {
    float tminx=1e9f,tmaxx=-1e9f,tminy=1e9f,tmaxy=-1e9f, sminx=1e9f,smaxx=-1e9f,sminy=1e9f,smaxy=-1e9f;
    int tn=0, sn=0;
    for (int i = 0; i < mSink->n; i++) {
      const RqItem& it = mSink->items[i];
      float *mnx,*mxx,*mny,*mxy; int* cnt;
      if (it.dbg_node == kTerrainDbgNode)          { mnx=&tminx; mxx=&tmaxx; mny=&tminy; mxy=&tmaxy; cnt=&tn; }
      else if (it.dbg_node == kSceneTableDbgNode)  { mnx=&sminx; mxx=&smaxx; mny=&sminy; mxy=&smaxy; cnt=&sn; }
      else continue;
      (*cnt)++;
      for (int k = 0; k < 4; k++) {
        if (it.xsf[k] < *mnx) *mnx = it.xsf[k]; if (it.xsf[k] > *mxx) *mxx = it.xsf[k];
        if (it.ysf[k] < *mny) *mny = it.ysf[k]; if (it.ysf[k] > *mxy) *mxy = it.ysf[k];
      }
    }
    fprintf(stderr, "[tier1sc] terrain n=%d bbox=[%.0f,%.0f,%.0f,%.0f] sceneTable n=%d bbox=[%.0f,%.0f,%.0f,%.0f]\n",
            tn, tn?tminx:0, tn?tminy:0, tn?tmaxx:0, tn?tmaxy:0, sn, sn?sminx:0, sn?sminy:0, sn?smaxx:0, sn?smaxy:0);
  }
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
// shadow map is never interpolated — render_queue.h sh_cast contract). Base on b (the newer real frame).
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
// TIER 1 EXCLUSION RULE (fps60.h "Object-tier attempt", extended to fieldEntityRender): RQ_WORLD prims
// tagged kTerrainDbgNode OR kSceneTableDbgNode (render_queue.h) are EXACTLY the prims Fps60::tier1Render
// re-renders — native_terrain.cpp and Render::fieldEntityRender each scope their own
// diag.beginObject(...)/endObject() around their quad loop, and BOTH the real per-logic-frame call and
// tier1Render's present-time re-invocation go through the same function body, so both tag identically.
// NOT the same set as "dbg_node==0" generally — conflating "terrain" with "any dbg_node==0 RQ_WORLD prim"
// was tried and produced a visible bug (grass vanishing from slot A, verified via
// scratch/check_tier1.py before that fix — every terrain-bbox pixel differed from the real neighbor
// because the ground behind the semi terrain quad had been excluded along with it). Explicit per-emitter
// sentinels are what let the exclusion target ONLY what tier1Render actually re-renders, emitter by
// emitter, as each one gets RE'd+ported per docs/fps60-rework.md's "REDIRECT" doctrine.
static constexpr uint32_t kTier1Sink = 0xFFFFFFFEu;
// BACKDROP = TIER 1 (LAYER-TRANSFORM lerp, docs/fps60-rework.md): the scrolling sky/parallax tilemap
// (Render::backdropRender) is screen-space, GAME-LOGIC-DRIVEN scroll — not camera-projected geometry — so
// it is never matched/lerped per-prim by the queue-lerp heuristic below; it is re-rendered as ONE layer
// through the SAME native pass with the scroll offset overridden to a lerp of the two real frames'
// captured offsets (Fps60::tier1Render), output landing in the same mSink as terrain/scene-table. Its
// identity is the RQ layer itself: RQ_BACKGROUND has exactly one producer (backdropRender — grep-verified,
// the only push2dQuad(RQ_BACKGROUND, ...) call site in the tree), so layer IS its real engine identity
// here; no separate dbg_node sentinel is needed (RQ_BACKGROUND items get dbg_node==0 from emitOrQueue
// regardless — layer already disambiguates them from every dbg_node==0 RQ_WORLD producer).
static inline bool isTier1Owned(const RqItem& it) {
  if (it.layer == RQ_BACKGROUND) return true;
  return it.layer == RQ_WORLD && (it.dbg_node == kTerrainDbgNode || it.dbg_node == kSceneTableDbgNode);
}
void Fps60::buildProvenanceIdx(const RqItem* items, int n, std::vector<uint32_t>& out) {
  out.resize((size_t)n);
  mNodeIdxScratch.clear();
  for (int i = 0; i < n; i++) {
    if (isTier1Owned(items[i])) { out[i] = kTier1Sink; continue; }
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
  // (handled by the fingerprint/order-of-appearance pass below); tier1-owned (kTier1Sink — terrain,
  // scene-table, AND backdrop, see isTier1Owned) items are excluded from the queue-lerp entirely —
  // tier1Render draws them (see buildProvenanceIdx).
  mMatchMap.clear();
  for (int j = 0; j < nB; j++)
    if (mIdxCurBuf[j] != kNoNode && mIdxCurBuf[j] != kTier1Sink)
      mMatchMap[((uint64_t)B[j].dbg_node << 32) | mIdxCurBuf[j]] = j;

  // dbg_node==0 groups, in each frame's own emission order, keyed by shape fingerprint.
  mZeroGroupsPrev.clear(); mZeroGroupsCur.clear();
  for (int i = 0; i < nA; i++) if (mIdxPrevBuf[i] == kNoNode) mZeroGroupsPrev[fpHash(A[i])].push_back(i);
  for (int j = 0; j < nB; j++) if (mIdxCurBuf[j]  == kNoNode) mZeroGroupsCur[fpHash(B[j])].push_back(j);

  mMatchOfA.assign((size_t)nA, -1);
  mUsedB.assign((size_t)nB, 0);

  // Pass 1 (key strength 1+2): provenance-keyed prims, validated by fingerprint + color on every hit.
  for (int i = 0; i < nA; i++) {
    if (mIdxPrevBuf[i] == kNoNode || mIdxPrevBuf[i] == kTier1Sink) continue;
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

  // Pass 3 — Tier-3 object-atomicity (docs/fps60-rework.md "QUEUE-LERP remains only for prims outside
  // tiers 1-2 ... Make its matching OBJECT-ATOMIC"). dbg_node==0 items have no object identity and are
  // exempt (Pass 2 above already gated them on a whole-group count match).
  enforceNodeAtomicity(nA);

  // Emit: walk Q[N-1] in its captured paint order. Tier1-owned (terrain/scene-table/backdrop) -> skip
  // (mSink already drew it, see tier1Render); matched -> lerp(A,B,t); unmatched -> A as-is.
  mNLerp = 0;
  for (int i = 0; i < nA && mNLerp < FPS60_RQ_MAX; i++) {
    if (mIdxPrevBuf[i] == kTier1Sink) continue;
    if (mMatchOfA[i] >= 0) { mRqLerp[mNLerp++] = lerpItem(A[i], B[mMatchOfA[i]], mT); mMatchedThisFrame++; }
    else                   { mRqLerp[mNLerp++] = A[i];                              mUnmatchedThisFrame++; }
  }
  mMatchedTotal += mMatchedThisFrame; mUnmatchedTotal += mUnmatchedThisFrame;
  // Match-rate telemetry: once/sec (logic runs ~30fps -> every 30 fences) under PSXPORT_DEBUG=fps60.
  if (mDbg && (mFence % 30) == 0) {
    long tot = mMatchedTotal + mUnmatchedTotal;
    fprintf(stderr, "[fps60] f%ld match-rate: this-frame matched=%ld/%d (%.0f%%) unmatched=%ld backdrop=%ld tier1=%ld | "
                     "running matched=%ld/%ld (%.0f%%)\n",
            mFence, mMatchedThisFrame, nA, nA ? 100.0 * mMatchedThisFrame / nA : 0.0, mUnmatchedThisFrame,
            mBackdropPrimsThisFrame, mTier1PrimsThisFrame,
            mMatchedTotal, tot, tot ? 100.0 * mMatchedTotal / tot : 0.0);
  }
}

// ⛔ TRANSITIONAL HACK DEBT (docs/fps60-rework.md "REDIRECT"): this whole file's match+lerp path is a
// queue-level HEURISTIC standing in for real per-element engine identity. As each quad-emitting guest fn
// is RE'd and ported native (camera/object transform tiers), its prims carry real identity and are
// interpolated by their OWNING tier instead — they are excluded from matchAndLerp entirely, and this
// heuristic's coverage (and this comment) shrinks toward zero, then deletes.
//
// Tier-3 object-atomicity: a dbg_node identifies one engine object's prims. A torn match — some of an
// object's prims lerp while a sibling prim (same object, same frame) fails to match — reads as the object
// partially "melting" into its neighbor's shape for one in-between frame. Demote the WHOLE node back to
// unmatched (draws Q[N-1] verbatim, like today's pop-in) rather than draw a torn mix.
void Fps60::enforceNodeAtomicity(int nA) {
  const RqItem* A = mRqPrev;
  mNodeTotalA.clear(); mNodeMatchedA.clear();
  for (int i = 0; i < nA; i++) {
    uint32_t node = A[i].dbg_node;
    if (!node) continue;                          // no provenance — not this tier's concern (Pass 2 above)
    mNodeTotalA[node]++;
    if (mMatchOfA[i] >= 0) mNodeMatchedA[node]++;
  }
  for (int i = 0; i < nA; i++) {
    uint32_t node = A[i].dbg_node;
    if (!node || mMatchOfA[i] < 0) continue;
    if (mNodeMatchedA[node] != mNodeTotalA[node]) {   // this node had at least one unmatched sibling prim
      mUsedB[(size_t)mMatchOfA[i]] = 0;
      mMatchOfA[i] = -1;
    }
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
// had.
void Fps60::present_vk(Core* core) {
  Core* c = core;
  RenderQueue& q = c->game->rq;
  if (mDbg < 0) mDbg = cfg_dbg("fps60") ? 1 : 0;

  // Gate-B test knob (TEMPORARY, internal — see docs/config.md): PSXPORT_FPS60_TFORCE=0 pins the whole
  // present (camera lerp + queue-prim lerp share mT) to Q[N-1]'s endpoint, =1 to Q[N]'s, so a run can be
  // pixel-diffed against the adjacent real frame to prove tier1Render/matchAndLerp ARE the real path at
  // the endpoints, not an approximation. Unset -> default 0.5 (the shipped midpoint).
  int tforce = cfg_int("PSXPORT_FPS60_TFORCE", -1);
  if (tforce == 0) mT = 0.0f; else if (tforce == 1) mT = 1.0f; else if (tforce < 0) mT = 0.5f;

  // ---- PASS 1 (slot A): TIER 1 (terrain, camera-lerped, into mSink) + lerp(Q[N-1], Q[N]) by matched prim
  // for everything tier1 doesn't own, Q[N-1] as-is otherwise -----------------------------------------------
  // First frame after enabling fps60 (no Q[N-1]/mCamPrev captured yet, mHavePrev==0): degenerate to
  // replaying THIS frame's own queue (Q[N] twice, terrain included) rather than matching/lerping against
  // an empty/garbage buffer — 30fps content at 60Hz pacing for exactly one frame (stage-1 first-frame case).
  const RqItem* slotA; int nSlotA;
  mTier1PrimsThisFrame = 0;
  if (mHavePrev) {
    // #50: only re-render the native field passes if the real frame actually ran them. During an authored
    // OT sub-scene (hut interior) the real frame did a full OT walk (no sceneNative), so the interior is
    // already in the captured queue (Q[N-1]/Q[N]) and matchAndLerp handles it — re-rendering the field here
    // would draw the exterior on interp frames only (the flicker). mSink stays empty; the merge below emits
    // just the lerped queue.
    if (mTier1EligibleCur) tier1Render(c, mT);   // fills mSink with the camera-lerped terrain re-render
    matchAndLerp(c);                    // fills mRqLerp with everything else (tier1-owned prims excluded)
    slotA = mRqLerp; nSlotA = mNLerp;
    // Merge-emit mSink with mRqLerp by (layer, seq) — NOT sink-then-slotA. mSink is all RQ_WORLD, its own
    // push()-assigned seq starting at 0 (terrain draws first among world items in the real per-logic-frame
    // walk too — render_walk.cpp's sceneNative order is (a) TERRAIN, (b) SCENE TABLE, (c) OBJECTS, (d)
    // TOMBA — so mSink's seq range is already the lowest within RQ_WORLD, same relative position as the
    // real frame). mRqLerp is already (layer,seq)-sorted (matchAndLerp walks Q[N-1] in ITS captured paint
    // order, which flush() sorted before capture). A naive sink-first-then-slotA emit order draws the
    // (semi, submission-order-composited) terrain quad against an EMPTY framebuffer instead of behind the
    // background it belongs behind — verified wrong via scratch/check_tier1.py before this fix (every
    // terrain-bbox pixel differed at t=1 from the real neighbor).
    // #50: mSink is nullptr when tier1Render was skipped (ineligible frame — authored sub-scene); treat as
    // empty so the merge just emits the lerped queue (mSink is lazily allocated only inside tier1Render).
    const int sinkN = mSink ? mSink->n : 0;
    int ia = 0, ib = 0;
    while (ia < sinkN || ib < nSlotA) {
      bool takeSink;
      if (ia >= sinkN) takeSink = false;
      else if (ib >= nSlotA) takeSink = true;
      else {
        const RqItem& sa = mSink->items[ia]; const RqItem& sb = slotA[ib];
        takeSink = (sa.layer != sb.layer) ? (sa.layer < sb.layer) : (sa.seq <= sb.seq);
      }
      if (takeSink) q.emitItem(c, &mSink->items[ia++]);
      else          q.emitItem(c, &slotA[ib++]);
    }
  } else {
    slotA = mRqCur;  nSlotA = mNCur;
    for (int i = 0; i < nSlotA; i++) q.emitItem(c, &slotA[i]);
  }
  gpu_fps60_present_pass(c);
  dumpPresent(c, /*interp=*/true);
  if (mDbg) fprintf(stderr, "[fps60] f%ld slotA: replay prev=%s n=%d tier1=%ld backdrop=%ld t=%.3f\n",
                    mFence, mHavePrev ? "Q[N-1]" : "Q[N] (first frame)", nSlotA, mTier1PrimsThisFrame,
                    mBackdropPrimsThisFrame, mT);
  gpu_pace_subframe(c, 2);

  // ---- PASS 2 (slot B): the real frame (the captured queue, exactly as drawOTag built it) ---------------
  for (int i = 0; i < mNCur; i++) q.emitItem(c, &mRqCur[i]);
  gpu_present_ex(c, 1);
  dumpPresent(c, /*interp=*/false);
  gpu_pace_subframe(c, 2);

  // ---- rotate captures: POINTER SWAP, not memcpy (avoids the double-copy churn of the old design) -------
  // mRqCur (just-drawn Q[N]) becomes next frame's mRqPrev; the buffer mRqPrev used to point at (now-stale,
  // two-frames-old content that's never read again) becomes rq_capture's next overwrite target. mCamCur/
  // mCamPrev and mBgCur/mBgPrev rotate in lockstep — both were written by THIS frame's own
  // terrainRenderAll()/backdropRender() calls (via sceneCam/bgScroll, during drawOTag, before this
  // present_vk call — see the invariant in fps60.h) so they hold the SAME frame's state mRqCur's content
  // came from.
  std::swap(mRqCur, mRqPrev);
  std::swap(mNCur, mNPrev);
  std::swap(mCamCur, mCamPrev);
  std::swap(mBgCur, mBgPrev);
  mHavePrev = 1;
}

