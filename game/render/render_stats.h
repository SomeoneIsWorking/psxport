// class RenderStats — per-Core render diag counters.
//
// A grab-bag of the small numeric counters the render path emits for diagnostics — obj-depth cache
// hits/misses (the packet-pool span cache in gpu_native.cpp), projprim / depth-cache hits/misses (the
// per-vertex depth cache in gte_beetle.cpp), the per-frame 3D-with-real-depth vs 2D-order-band prim
// split (native-depth ndepth diag), the scene-native walk counters, and the SBS world-quads diag.
//
// Per-Core because gpu_native.cpp / gte_beetle.cpp already run per-instance (Beetle GTE + GpuState are
// bound per Core at frame step), so two cores in SBS produce independent frames — the diag counters
// they emit should be independent too. Was a scatter of process-globals (g_od_add/hit/miss,
// g_pp_set/hit/miss, g_nd_3d/nd_2d, g_sn_objs/cmds, g_dbg_world_quads); deglobalize-game 2026-07-03.
//
// Reached as `core->mRender->stats`. All fields are public counters — increments are `stats.odAdd++`
// (thin OOP for what really is just a numeric accumulator); the class earns its keep by grouping the
// related counters and owning the dump() format the render path prints per-frame.
#pragma once
#include <stdio.h>

class RenderStats {
public:
  // Per-frame prim counts, native-depth (ndepth) diag: 3D prims drawn with a real depth vs 2D prims
  // that fell to the deferred OT-order band. Reset at frame start.
  long nd3d = 0;
  long nd2d = 0;

  // Object-depth span cache diag (packet-pool [lo,hi) -> world-depth records + lookups by 2D billboard
  // recovery, gpu_native.cpp). Reset at frame start via the s_od_frame guard.
  long odAdd  = 0;
  long odHit  = 0;
  long odMiss = 0;

  // (ProjPrim / depth-cache diag lives in gte_beetle.cpp as file-scope statics — the projprim cache
  // itself is process-wide, so those counters are too. Read via projprim_stats_read/reset.)

  // Scene-native walk counters (game/render/engine_render_walk.cpp).
  long snObjs = 0;
  long snCmds = 0;

  // World-quads diag: prims classified as RQ_WORLD emitted this frame. Read by the SBS "black-pane"
  // investigation and the PSXPORT_GPU_TRACE dump.
  long dbgWorldQuads = 0;
};
