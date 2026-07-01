// Shared internals of the native render path — split out so the geometry-SUBMIT subsystem
// (engine_submit.cpp: poly submitters, the render-command dispatcher, transform/matrix orchestration)
// and the render-list WALK subsystem (render_walk.cpp: ov_scene_native + the master/snapshot/aux list
// walks + per-object render/flush + the native backdrop) can live in separate files while sharing the
// few helpers both need (per-object depth tagging, the nesting-safe packet-span session, and the native
// generic GT3/GT4 submit the per-object flush calls).
#pragma once
#include "core.h"
#include "mods.h"   // g_mods (objid/debug-ids gate for the fps60 billboard recorder)
#include "cfg.h"    // cfg_dbg

// --- per-object depth helpers (the engine owns object depth from the object's real world placement) ---
void  gpu_obj_depth_add(Core*, uint32_t lo, uint32_t hi, float ord);
float proj_camview_world_ord(float wx, float wy, float wz);
int   camview_valid(void);
float proj_obj_center_ord(void);
void  fps60_record_billboard_span(Core* c, uint32_t lo, uint32_t hi, uint32_t ident);
extern int g_fps60_on;

// The entity node the native render walk is currently rendering (set around each per-object dispatch). The
// PER-INSTANCE identity for every prim an object emits — including a 2D billboard whose quad rasterizes
// later at the OT walk. Shared with gpu_native (objid overlay) and both render subsystems.
extern uint32_t g_dbg_render_node;

// The real per-instance render object: the walk's node when set, else the guest "current render object"
// scratch (0x1F80028C). Prefer the native walk's node — 0x28C is shared/stale for some billboard paths.
static inline uint32_t cur_render_node(Core* c) {
  return g_dbg_render_node ? g_dbg_render_node : c->mem_r32(0x1F80028Cu);
}

// The engine's PC-native depth for an object: project its REAL spawned WORLD position (node+0x2e/0x32/0x36)
// through the STABLE scene camera, so render order can no longer leak into depth. Falls back to the live-GTE
// origin projection only before the scene camera is known (first frame / no terrain in scene).
static inline float obj_world_ord(Core* c, uint32_t node) {
  if (node && camview_valid()) {
    float wx = (float)(int16_t)c->mem_r16(node + 0x2E);
    float wy = (float)(int16_t)c->mem_r16(node + 0x32);
    float wz = (float)(int16_t)c->mem_r16(node + 0x36);
    return proj_camview_world_ord(wx, wy, wz);
  }
  return proj_obj_center_ord();
}

// fps60: record the billboard entry mirroring a just-published gpu_obj_depth_add(span, node-depth).
static inline void fps60_bb_node(Core* c, uint32_t lo, uint32_t hi, uint32_t node) {
  if (g_fps60_on || g_mods.debug_ids || cfg_dbg("objid")) fps60_record_billboard_span(c, lo, hi, node);
}

// NESTING-SAFE packet-pool span tracking (issue #4 — ropes/flames drew over terrain). Per-object depth
// tagging captures the address span an object's renderer writes into the packet pool (g_pkt_track/lo/hi in
// mem.cpp), then stamps it with the object's world depth. A session SAVES the outer session's state on open
// and, on close, RESTORES it while MERGING its own [lo,hi) into the outer — so a renderer that internally
// dispatches the universal render command (ov_render_cmd) for the same node keeps the outer walk's tracking
// alive and the outer's final gpu_obj_depth_add covers ALL of the object's packets.
struct PktSpanSession {
  int outer_track; uint32_t outer_lo, outer_hi;
  PktSpanSession() {
    extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
    outer_track = g_pkt_track; outer_lo = g_pkt_lo; outer_hi = g_pkt_hi;
    g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
  }
  // Close the session: returns 1 + fills lo/hi if this session captured a non-empty span (caller does the
  // gpu_obj_depth_add with its own ord). Restores + merges into the outer session.
  int close(uint32_t* lo, uint32_t* hi) {
    extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
    uint32_t my_lo = g_pkt_lo, my_hi = g_pkt_hi;
    g_pkt_track = outer_track;                                   // resume the OUTER session (key fix)
    g_pkt_lo = outer_lo; g_pkt_hi = outer_hi;
    if (my_hi > my_lo) {                                          // merge my writes into the outer span
      if (my_lo < g_pkt_lo) g_pkt_lo = my_lo;
      if (my_hi > g_pkt_hi) g_pkt_hi = my_hi;
      if (lo) *lo = my_lo; if (hi) *hi = my_hi; return 1;
    }
    return 0;
  }
};

// --- cross-subsystem entry points ---
// Fully-native generic GT3/GT4 submit (engine_submit.cpp). The per-object flush in the walk calls it directly.
void native_gt3gt4(Core* c, uint32_t geomblk, uint32_t otbase);
// Scene-table (0x800F2418) world-coord render (engine_submit.cpp, uses the static poly submitters there).
void ov_field_entity_render(Core* c);
