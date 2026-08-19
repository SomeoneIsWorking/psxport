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
// Reached as `core->rsub.stats`. All fields are public counters — increments are `stats.odAdd++`
// (thin OOP for what really is just a numeric accumulator); the class earns its keep by grouping the
// related counters and owning the dump() format the render path prints per-frame.
#pragma once
#include <stdio.h>

class Core;

// THE DEPTH-COVERAGE REPORT — print it at RUN END, once, with its denominator.
//
// "Does this port have real per-primitive depth, and for how much of what it draws" is the question
// the 2D-vs-3D discriminator (widescreen re-centering, 60fps interpolation) rides on, and until now
// the only answer available was a one-frame sample that printed `3D%=0.0` whether it meant "no 3D"
// or "nothing counted". This prints the whole run: the 3D/2D prim split with the total it is a
// fraction of, and the vertex-depth cache's records/hits/misses over the same window. If nothing was
// drawn at all it SAYS SO instead of printing a percentage of zero — a coverage line that cannot
// distinguish "measured 0%" from "never measured" is the failure it exists to avoid.
//
// A port calls this from its own run-end path (the framework's frame loop is not entered by every
// port, so there is no framework-side hook that reliably runs).
void render_depth_coverage_report(Core* core, const char* why);

class RenderStats {
public:
  // Per-frame prim counts, native-depth (ndepth) diag: 3D prims drawn with a real depth vs 2D prims
  // that fell to the deferred OT-order band. Reset at frame start.
  long nd3d = 0;
  long nd2d = 0;

  // LIFETIME totals of the same split, and they exist because the per-frame pair could not answer
  // the question everyone asked it. The `ndepth` channel's line samples one frame in sixty while the
  // counters above are cleared on EVERY present, so 59 of 60 frames were discarded unread and the
  // printed percentage described a single frame — one that, on an alternate-field renderer, was
  // reliably a non-drawing one. Worse, its percentage is computed as `(a+b) ? … : 0.0`, so a frame
  // that counted NOTHING printed `3D%=0.0`, byte-identical to a frame that genuinely drew no 3D.
  // (spyro instrument I041, distrusted on exactly that.)
  //
  // These are never reset. A coverage number needs a denominator and a window, and the only honest
  // window for "does this port have real depth" is the whole run — so render_depth_coverage_report()
  // prints these with the prim total, and says "no prims at all" rather than printing a percentage
  // of nothing.
  long long nd3dTotal = 0;
  long long nd2dTotal = 0;

  // (ProjPrim / depth-cache diag now lives on `class ProjPrim` (game/render/proj_prim.h), embedded on
  // Render as `c->rsub.projprim` — bound via `ProjPrim::bind(c)` alongside gte_bind. Two SBS cores
  // keep separate caches + counters. Read via `.stats()` / `.statsReset()`.)

  // Scene-native walk counters (game/render/render_walk.cpp).
  long snObjs = 0;
  long snCmds = 0;

  // World-quads diag: prims classified as RQ_WORLD emitted this frame. Read by the SBS "black-pane"
  // investigation and the PSXPORT_GPU_TRACE dump.
  long dbgWorldQuads = 0;
};
