// Shared internals of the native render path — split out so the geometry-SUBMIT subsystem
// (submit.cpp: poly submitters, the render-command dispatcher, transform/matrix orchestration)
// and the render-list WALK subsystem (render_walk.cpp: ov_scene_native + the master/snapshot/aux list
// walks + per-object render/flush + the native backdrop) can live in separate files while sharing the
// few helpers both need (per-object depth tagging, the nesting-safe packet-span session, and the native
// generic GT3/GT4 submit the per-object flush calls).
#pragma once
#include "core.h"
#include "mods.h"   // g_mods
#include "cfg.h"    // cfg_dbg
#include <stdio.h>  // sil_bbox_log diag fprintf

// --- per-object depth helpers (the engine owns object depth from the object's real world placement) ---
void  gpu_obj_depth_add(Core*, uint32_t lo, uint32_t hi, float ord);
// --- UI-span registry (bug #34, docs/findings/ui.md "Dialog text-box PANEL emitter chain") ---
// Presence-only provenance: a dialog-box panel poly's packet-pool span, so the field's 2D-only OT walk
// keeps it as RQ_HUD instead of dropping it with the redundant native-owned world polys. No depth/ord —
// distinct from gpu_obj_depth_add's world-position billboard occlusion.
// --- NATIVE-COVER registry (docs/fps60-rework.md REDIRECT) --- see gpu_native_internal.h for the
// full rationale: marks a packet-pool span whose geometry was ALSO drawn through the real per-object
// float path this frame, so the field's 2D-only OT walk drops the substrate's redundant copy instead
// of billboard-promoting it.
void  gpu_native_cover_add(Core*, uint32_t lo, uint32_t hi);
float proj_obj_center_ord(void);
// class ProjParams (game/render/proj_params.h) — per-Core; brings in camview_valid/proj_camview_world_ord etc.
#include "proj_params.h"
// g_fps60_on retired — read g_mods.fps60 (mods.h)

// g_dbg_render_node retired 2026-07-02 — per-Core Render::mDbgRenderNode (set around each per-object
// dispatch in the native render walk; PER-INSTANCE identity for every prim an object emits, incl.
// billboards rasterized later at the OT walk).
#include "render.h"    // Render (needed for cur_render_node below)
#include "game.h"      // c->game->oracle
#include "pkt_span.h"  // PktSpanSession (withDepthTag below)
#include "render_queue.h"  // RenderQueue::emitOrQueue + RQ_WORLD (rq_push_ft4_record below)

// The real per-instance render object: the walk's node when set, else the guest "current render object"
// scratch (0x1F80028C). Prefer the native walk's node — 0x28C is shared/stale for some billboard paths.
static inline uint32_t cur_render_node(Core* c) {
  return c->mRender->diag.currentNode() ? c->mRender->diag.currentNode() : c->mem_r32(0x1F80028Cu);
}

// render_field_native_active: true iff pc_render's native field pass (Render::sceneNative + the
// twoDOnly OT walk, game_tomba2.cpp Engine::drawOTag) owns THIS frame's picture — GAME stage,
// free-roam (not SOP intro narration), pc_render (not psx_render), not the oracle. Any OTHER
// picture-producing addition that wants to draw real geometry natively (e.g. cmdListDispatch's
// generic-overlay REDIRECT, perobj_dispatch.cpp) must gate on this SAME condition: outside this
// window the guest OT's full walk (twoDOnly=false) is the sole picture source, so an extra native
// draw would double-draw. Deliberately narrower than drawOTag's own `scenenative` diagnostic branch
// (that debug channel stays diagnostic-only; it must not also arm new native draws).
static inline bool render_field_native_active(Core* c) {
  if (c->game->oracle || c->mRender->mode.psxRender()) return false;
  if (c->mem_r32(0x801FE00Cu) != 0x8010637Cu) return false;         // GAME stage resident
  if (c->mem_r32(0x80109450u) == 0x3C021F80u) return false;         // SOP intro narration overlay active
  // #51: an AUTHORED OT sub-scene (hut/door interior, sm[0x4c]==3 — the game's own fieldRunX/frameX
  // selector; see game_tomba2.cpp) is drawn ENTIRELY by the full guest-OT walk (#49), NOT the native
  // field pass — so the native pass does NOT own the picture here. If this returned true, the #48
  // native-cover + the cmdListDispatch REDIRECT would drop owned objects (Tomba, NPCs) from the OT walk
  // while their native draw never runs (sceneNative is gated off) -> the objects vanish (Tomba invisible).
  uint32_t task_sm = c->mem_r32(0x1F800138u);
  if (task_sm && c->mem_r16(task_sm + 0x4Cu) == 3) return false;
  return true;
}

// The engine's PC-native depth for an object: project its REAL spawned WORLD position (node+0x2e/0x32/0x36)
// through the STABLE scene camera, so render order can no longer leak into depth. Falls back to the live-GTE
// origin projection only before the scene camera is known (first frame / no terrain in scene).
static inline float obj_world_ord(Core* c, uint32_t node) {
  if (node && camview_valid()) {
    float wx = (float)c->mem_r16s(node + 0x2E);
    float wy = (float)c->mem_r16s(node + 0x32);
    float wz = (float)c->mem_r16s(node + 0x36);
    return proj_camview_world_ord(wx, wy, wz);
  }
  return proj_obj_center_ord();
}

// rq_push_ft4_record — the shared #65 dual-emit: push one 10-word FT4 billboard record (the layout
// QuadRtptSubmit::submitQuad and Render::billboardEmit both build: +4 rgb|code, +8/+16/+24/+32 SXY,
// +12 clut|v0u0, +20 tpage|v1u1, +28/+36 v2u2/v3u3) to the native render queue as a WORLD prim with
// a flat per-quad depth ord (the #28 billboard convention, natively). Reads the record the native
// emitter just built — its OWN values, not a packet transcription. No-op under psx_render/oracle
// (the guest OT walk draws the packets there). Host-only.
static inline void rq_push_ft4_record(Core* c, uint32_t rec, float ord) {
  if (c->game->oracle || c->mRender->mode.psxRender()) return;
  int xs[4], ys[4], us[4], vs[4];
  static const uint32_t sxyOff[4] = { 8, 16, 24, 32 }, uvOff[4] = { 12, 20, 28, 36 };
  for (int i = 0; i < 4; i++) {
    const uint32_t w = c->mem_r32(rec + sxyOff[i]);
    xs[i] = (int16_t)(w & 0xFFFFu);
    ys[i] = (int16_t)(w >> 16);
    const uint32_t uvw = c->mem_r32(rec + uvOff[i]);
    us[i] = uvw & 0xFFu;
    vs[i] = (uvw >> 8) & 0xFFu;
  }
  const uint32_t colorWord = c->mem_r32(rec + 4);
  const uint8_t  op   = (uint8_t)(colorWord >> 24);
  const uint32_t clut = c->mem_r32(rec + 12) >> 16;
  const uint32_t tp   = c->mem_r32(rec + 20) >> 16;
  unsigned char rs[4], gs[4], bs[4];
  for (int i = 0; i < 4; i++) {
    rs[i] = (unsigned char)(colorWord & 0xFF);
    gs[i] = (unsigned char)((colorWord >> 8) & 0xFF);
    bs[i] = (unsigned char)((colorWord >> 16) & 0xFF);
  }
  float dep[4] = { ord, ord, ord, ord };
  c->game->activeRq().emitOrQueue(c, /*capture=*/1, RQ_WORLD, RQ_OM_DEPTH, /*nv=*/4,
                                  /*semi=*/(op & 2) ? 1 : 0, /*raw=*/(op & 1) ? 1 : 0,
                                  xs, ys, nullptr, nullptr, us, vs, rs, gs, bs, dep,
                                  /*mode=*/(int)((tp >> 7) & 3u),
                                  (int)(tp & 0xFu) * 64, (int)((tp >> 4) & 1u) * 256,
                                  (int)(clut & 0x3Fu) * 16, (int)((clut >> 6) & 0x1FFu),
                                  0, 0, 0, 0, 0, 0, 1023, 511, (int)((tp >> 5) & 3u));
}

// Guest-transparent depth-tag wrap (RenderObserver's obs_body, folded in): PSXPORT_ORACLE runs `body`
// pure (core B / psx_fallback stays the untouched reference), everyone else opens a nested
// PktSpanSession around `body` and tags the packet span it emits with the object's PC-native world
// depth — so the field tee (s_ot_2d_only) KEEPS the resulting is3d=0 prims instead of dropping them for
// lack of a depth tag (#39: weapon chain + impact effect). Shared by every per-node dispatch site that
// reaches an otherwise-untagged custom renderer (perObjRenderDispatch's CCA4 body, renderWalk's
// 0x8003C29C RCASE_DEFAULT body).
static inline void withDepthTag(Core* c, uint32_t node, void (*body)(Core*)) {
  if (c->game->oracle) { body(c); return; }
  c->mRender->diag.beginObject(node);
  uint32_t slo, shi;
  PktSpanSession sess(c);
  body(c);
  if (sess.close(&slo, &shi)) {
    float od = obj_world_ord(c, node);
    gpu_obj_depth_add(c, slo, shi, od);
  }
  c->mRender->diag.endObject();
}

// PktSpan (per-Core packet-pool store-address-span tracker) + PktSpanSession (RAII object scope) live
// in pkt_span.h — reached as c->mRender->pktSpan. PktSpanSession is defined there; its ctor/close
// implementation is in pkt_span.cpp.

// Fully-native generic GT3/GT4 submit is Render::gt3gt4 (submit.cpp); the per-object flush in
// the walk calls it directly. Scene-table (0x800F2418) world-coord render is Render::fieldEntityRender.

// DIAG (debug channel "silbbox", scratch/handoff.md 2026-07-01 "dark outline" investigation): log the
// screen bbox of any drawn quad overlapping the known repro window (coastal-ridge dark silhouette line,
// pixel-measured x=5..30 y=134-138 — see the handoff). Every quad submitter (native_terrain, the GT3/GT4
// library, the byte-packed variant, and the sky/backdrop tilemap) should call this so the next session can
// see which pass(es) DO or DON'T cover that region — the hypothesis is a sub-pixel coverage gap between
// the hillside object's quad(s) and the sky backdrop letting the black clear color show through.
static inline void sil_bbox_log(const char* tag, const float* px, const float* py, int n) {
  if (!cfg_dbg("silbbox")) return;
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < n; i++) {
    if (px[i] < minx) minx = px[i]; if (px[i] > maxx) maxx = px[i];
    if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i];
  }
  if (maxx < -20 || minx > 160 || maxy < 100 || miny > 200) return;   // outside the repro window, skip
  fprintf(stderr, "[silbbox] %s bbox x=[%.1f,%.1f] y=[%.1f,%.1f]\n", tag, minx, maxx, miny, maxy);
}
// Same, but also identifies WHICH entity node emitted the quad (cur_render_node(c) at call time) — use
// at per-object submit sites so overlapping bboxes at the repro window can be traced back to the object.
static inline void sil_bbox_log_node(const char* tag, const float* px, const float* py, int n, uint32_t node) {
  if (!cfg_dbg("silbbox")) return;
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < n; i++) {
    if (px[i] < minx) minx = px[i]; if (px[i] > maxx) maxx = px[i];
    if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i];
  }
  if (maxx < -20 || minx > 160 || maxy < 100 || miny > 200) return;
  fprintf(stderr, "[silbbox] %s node=%08X bbox x=[%.1f,%.1f] y=[%.1f,%.1f]\n", tag, node, minx, maxx, miny, maxy);
}
static inline void sil_bbox_log_i(const char* tag, const int* xs, const int* ys, int n) {
  if (!cfg_dbg("silbbox")) return;
  float pxf[8], pyf[8]; n = n > 8 ? 8 : n;
  for (int i = 0; i < n; i++) { pxf[i] = (float)xs[i]; pyf[i] = (float)ys[i]; }
  sil_bbox_log(tag, pxf, pyf, n);
}
// Same repro-window gate as sil_bbox_log_node, but also dumps every vertex's screen coord + depth and the
// source record address, so a coverage gap can be told apart from a wrong-color draw (2026-07-01 dark-outline
// direct-inspection pass, scratch/handoff.md).
static inline void sil_bbox_log_verts(const char* tag, const float* px, const float* py, const float* depth,
                                       int n, uint32_t node, uint32_t rec_addr,
                                       const uint8_t* r = nullptr, const uint8_t* g = nullptr, const uint8_t* b = nullptr) {
  if (!cfg_dbg("silbbox")) return;
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < n; i++) {
    if (px[i] < minx) minx = px[i]; if (px[i] > maxx) maxx = px[i];
    if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i];
  }
  if (maxx < -20 || minx > 160 || maxy < 100 || miny > 200) return;
  fprintf(stderr, "[silbbox] %s node=%08X rec=%08X bbox x=[%.1f,%.1f] y=[%.1f,%.1f] verts:",
          tag, node, rec_addr, minx, maxx, miny, maxy);
  for (int i = 0; i < n; i++) {
    fprintf(stderr, " (%.2f,%.2f,z=%.4f", px[i], py[i], depth[i]);
    if (r) fprintf(stderr, ",rgb=%d,%d,%d", r[i], g[i], b[i]);
    fprintf(stderr, ")");
  }
  fprintf(stderr, "\n");
}
