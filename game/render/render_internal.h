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
void  gpu_ui_span_add(Core*, uint32_t lo, uint32_t hi);
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
