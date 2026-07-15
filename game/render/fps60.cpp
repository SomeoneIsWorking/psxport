// fps60 — renderer-internal, one-frame-behind interpolated-60fps tier (docs/fps60-rework.md; UNIFIED-PATH
// redesign 2026-07-15, USER: "no difference between real and interpolated frames aside from lerp"). See
// fps60.h. In one sentence: the interp frame is the SAME render re-run one frame behind, with the lerp
// living entirely in the INPUTS — camera (sceneCam), per-object transforms (projObj), and backdrop scroll
// (bgScroll) each served a lerp(prev,cur,t) at present time; tier1Render re-runs the field world
// (terrain+scene-table+objects+backdrop) into mSink under those lerped inputs, and present_vk merges it
// with mRqCur's 2D (HUD/overlay, screen-space, verbatim). The authored sub-scene (hut) presents its
// captured interior (mRqCur). No prim matching (matchAndLerp deleted). Host-only, gated g_mods.fps60.
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

Fps60::~Fps60() { delete[] mRqCur; delete[] mRqPrev; delete mSink; }

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
  // mSink is a bare capture sink (not one of Game's constructor-wired RenderQueue members — see
  // Game::Game's `rq.game = this` — so its own `game` back-pointer is null unless wired here). #54:
  // that was harmless while every push2dQuad(RQ_BACKGROUND, ...) call unconditionally forced
  // dbg_node=0 (never dereferencing `game`) — extending that to also stamp kBackdropDbgNode via
  // `core->mRender->diag.currentNode()` (render_queue.cpp emitOrQueue) exposed it: push2dQuad resolves
  // its OWN Core internally as `&game->core` (it takes no Core* param), so a null `game` there crashed
  // on the very next real-frame backdrop re-render. Wire it once, same as every other Game-owned queue.
  if (!mSink) { mSink = new RenderQueue(); mSink->game = game; }
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
    // fps60 step 2b: re-run the field OBJECT walk under lerped per-object transforms (mObjOverrideOn +
    // step-1's projObj) AND the still-armed lerped camera (mCamOverrideOn) into mSink — the objects
    // interpolate through the SAME object walk the real frame ran, replacing matchAndLerp's output-
    // matching for field actors. Only the field runs this (tier1Render is mTier1EligibleCur-gated).
    mObjOverrideOn = true;
    c->mRender->fieldObjectsRender();
    mObjOverrideOn = false;
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


// Per-frame node-emission-index: walk items in their captured (already paint-ordered) sequence, counting
// per dbg_node — objects emit their prims in stable order frame-to-frame (submit.cpp's per-object render
// walk), so (dbg_node, running count) is a stable identity while the object lives. dbg_node==0 (no
// provenance: terrain/static/background prims, or a billboard the OT walk couldn't stamp) gets the "no
// node" sentinel — those are matched separately, by fingerprint + order-of-appearance (key strength 3).
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
// BACKDROP = TIER 1 (LAYER-TRANSFORM lerp, docs/fps60-rework.md): the scrolling sky/parallax tilemap
// (Render::backdropRender) is screen-space, GAME-LOGIC-DRIVEN scroll — not camera-projected geometry — so
// it is never matched/lerped per-prim by the queue-lerp heuristic below; it is re-rendered as ONE layer
// through the SAME native pass with the scroll offset overridden to a lerp of the two real frames'
// captured offsets (Fps60::tier1Render), output landing in the same mSink as terrain/scene-table.
// #54 CORRECTION: "RQ_BACKGROUND has exactly one producer" was FALSE — the generic guest-OT walk
// (runtime/recomp/gpu_native.cpp) also classifies any full-screen 2D poly/sprite/FillRect as RQ_BACKGROUND
// by screen coverage (menu backdrop art, hut-interior clear, SOP-narration fills, #52's FillRect widen),
// completely unrelated to backdropRender. Blanket-excluding the whole layer dropped THOSE prims from every
// interpolated frame with no fallback (tier1Render never re-renders them — mTier1EligibleCur is field-
// only) — the title-menu screen went backdrop-less at 60fps, only its HUD text surviving. Fixed: only
// backdropRender's OWN prims (tagged kBackdropDbgNode, render_walk.cpp) are tier1-owned; every other
// RQ_BACKGROUND item keeps dbg_node==0 and falls through to the normal per-prim match+lerp/verbatim-
// fallback path below, same as any other un-owned 2D content.
static inline bool isTier1Owned(const RqItem& it) {
  // fps60 step 2b: tier1Render now re-renders ALL of RQ_WORLD (terrain + scene-table + OBJECTS via
  // fieldObjectsRender) under lerped inputs, so the whole world layer is tier1-owned and excluded from
  // matchAndLerp — objects come from the re-run's lerped transforms, not the output-match heuristic.
  // (Backdrop: only backdropRender's own prims; other RQ_BACKGROUND 2D keeps the per-prim path, #54.)
  if (it.layer == RQ_BACKGROUND) return it.dbg_node == kBackdropDbgNode;
  return it.layer == RQ_WORLD;
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
  if (mHavePrev && mSubsceneCur) {
    // fps60 step 2a — AUTHORED SUB-SCENE (hut/door interior, sm[0x4c]==3): the real frame drew the interior
    // via the guest-OT walk (drawOTag's authored_subscene branch), captured into mRqCur. The tier1+
    // matchAndLerp slot-A build drew the stale field instead (the flicker). Present the captured interior
    // (mRqCur) as slot A: interp == the interior, no flicker. Degenerate lerp — a guest OT has no native
    // per-object transform to interpolate (that needs the emitters RE'd/ported), so the sub-scene interp
    // frame is the real interior content, which is exactly right (no motion to lerp = interp == real).
    slotA = mRqCur; nSlotA = mNCur;
    for (int i = 0; i < nSlotA; i++) q.emitItem(c, &slotA[i]);
  } else if (mHavePrev) {
    // fps60 UNIFIED PATH (docs/fps60-rework.md): the field frame's WORLD — terrain + scene-table + OBJECTS
    // + backdrop — is re-run by tier1Render into mSink under the lerped camera + per-object transforms, ALL
    // interpolated through the SAME render path the real frame used (lerp lives in the INPUTS, not a per-
    // prim output match). Slot A = mSink (that lerped world) merged with mRqCur's REMAINING prims — the 2D
    // HUD/overlay, which are screen-space and presented VERBATIM (no lerp needed) — in (layer, seq) paint
    // order. isTier1Owned marks the world prims that come from mSink so we skip them in mRqCur. No
    // matchAndLerp: nothing is fingerprint-matched anymore. (mTier1EligibleCur gates the field re-render;
    // when false — narration — mSink stays empty and only the 2D prims emit.)
    if (mTier1EligibleCur) tier1Render(c, mT);
    const int sinkN = mSink ? mSink->n : 0;
    int ia = 0, ib = 0;
    for (;;) {
      while (ib < mNCur && isTier1Owned(mRqCur[ib])) ib++;   // world prims come from mSink — skip in mRqCur
      const bool haveA = ia < sinkN, haveB = ib < mNCur;
      if (!haveA && !haveB) break;
      bool takeSink;
      if (!haveB)      takeSink = true;
      else if (!haveA) takeSink = false;
      else {
        const RqItem& sa = mSink->items[ia]; const RqItem& sb = mRqCur[ib];
        takeSink = (sa.layer != sb.layer) ? (sa.layer < sb.layer) : (sa.seq <= sb.seq);
      }
      if (takeSink) q.emitItem(c, &mSink->items[ia++]);
      else          q.emitItem(c, &mRqCur[ib++]);
    }
    slotA = mRqCur; nSlotA = mNCur;   // telemetry only
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
  std::swap(mObjCur, mObjPrev);   // this frame's per-object transforms become next frame's Q[N-1]
  mObjCur.clear();                // fresh capture set for the next real frame's projComposeObject calls
  mHavePrev = 1;
}

// PER-OBJECT TRANSFORM choke (docs/fps60-rework.md unified-path redesign). Real projComposeObject call
// (mObjOverrideOn false): read Robj (cmd+0x18) / Tobj (cmd+0x2C) live from guest RAM and CAPTURE them
// keyed by cmd — byte-identical to the old direct read (this is the read the old code did inline), plus
// the host-memory capture. Interp present (mObjOverrideOn true, set by the interp re-run): return
// lerp(mObjPrev[cmd], mObjCur[cmd], mT) — the object interpolated through the SAME render path. A newly
// appeared object (in cur, no prev) uses cur as-is (no lerp) for its first frame; a stale key (in prev,
// not cur) is simply not re-rendered (the object walk only visits live cmds). When fps60 is off, the
// override is never armed and the capture map churn is the only cost.
void Fps60::projObj(Core* c, uint32_t cmd, float Robj[3][3], float Tobj[3]) {
  if (mObjOverrideOn) {
    auto pc = mObjCur.find(cmd);
    if (pc != mObjCur.end()) {
      auto pp = mObjPrev.find(cmd);
      const Fps60Obj& C = pc->second;
      if (pp != mObjPrev.end()) {
        const Fps60Obj& P = pp->second;
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) Robj[i][j] = P.R[i][j] + (C.R[i][j] - P.R[i][j]) * mT;
          Tobj[i] = P.T[i] + (C.T[i] - P.T[i]) * mT;
        }
      } else {  // new object this frame — no prev to lerp from, use cur
        for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) Robj[i][j] = C.R[i][j]; Tobj[i] = C.T[i]; }
      }
      return;
    }
    // cmd not captured this frame (shouldn't happen for a live-walked object) — fall through to a live read.
  }
  // Real frame: read live from guest RAM (the exact read projComposeObject used to do inline).
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++)
      Robj[row][col] = (float)c->mem_r16s(cmd + 0x18u + (uint32_t)col * 2u + (uint32_t)row * 6u);
  Tobj[0] = (float)c->mem_r16s(cmd + 0x2Cu);
  Tobj[1] = (float)c->mem_r16s(cmd + 0x30u);
  Tobj[2] = (float)c->mem_r16s(cmd + 0x34u);
  // Capture keyed by cmd (host memory only — the READ-ONLY OVERLAY invariant holds).
  Fps60Obj o;
  for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) o.R[i][j] = Robj[i][j]; o.T[i] = Tobj[i]; }
  mObjCur[cmd] = o;
}

