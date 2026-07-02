// class Render — the PC-native RENDER SUBSYSTEM umbrella owned by Core.
//
// PROPER OOP: one instance per Core, reached as `c->mRender->...`. This class exists to group the
// per-Core render-side subsystems (currently just NodeXform; scene-graph submit / walk / project etc.
// will migrate in here over time, out of the engine_submit.cpp / engine_render*.cpp grab-bags).
//
// Owned by Core via a POINTER (`Core::mRender`) — construction/destruction lives in the Core ctor/dtor
// in runtime/recomp/core.cpp; back-pointer `mCore` is wired there, and each embedded sub-subsystem's
// own back-pointer is wired there too. Callers reach members as `c->mRender->mNodeXform.build(node)`.
#pragma once
#include "node_xform.h"
class Core;

class Render {
public:
  Core* mCore = nullptr;

  // Route the field render through the PSX recomp path instead of the native scene-walk. A/B compare
  // switch — one bool per Core (SBS/dualcore set it per-core; standalone set once at boot from
  // PSXPORT_RENDER_PSX; live REPL toggle `renderpsx on|off`). Was the process-global `int g_render_psx`,
  // which contaminated both cores in-process (SBS "both cores same" symptom, deglobalize-game 2026-07-02).
  bool mPsxRender = false;
  bool psxRender() const { return mPsxRender; }
  void setPsxRender(bool on) { mPsxRender = on; }

  // Dual-view: render ONE game state two ways side-by-side (engine-native left | PSX-recomp right).
  // Set from PSXPORT_DUALVIEW at boot; gates the second (PSX) render pass in native_step_frame and the
  // GPU's two-batch allocation. Per-Core — was the process-global `int g_dualview` (deglobalize-game 2026-07-02).
  bool mDualview = false;
  bool dualview() const { return mDualview; }
  void setDualview(bool on) { mDualview = on; }

  // Per-object diag scope — set/cleared around each per-object dispatch in the native render walk so
  // downstream code (fps60 billboard recorder, render queue tagging, sil_bbox_log diag) can identify
  // which live object emitted a prim. `mDbgRenderNode` = the object's node pointer (0 outside object
  // scope); `mDbgCurGeomblk` = the geomblk chunk currently in native_gt3gt4. Per-Core — were the
  // process-globals g_dbg_render_node / g_dbg_cur_geomblk (deglobalize-game 2026-07-02).
  uint32_t mDbgRenderNode  = 0;
  uint32_t mDbgCurGeomblk  = 0;

  // Packet-tracking session: while armed (during one render-command dispatch — engine_submit's
  // ov_render_cmd, or a bdtag span wrapper), record the address span of stores landing in the
  // packet/OT pool [0x800BFE68,0x800E7E68). Read by Core::mem_w* via this->mRender. The span is
  // then tagged with the object's world-position depth so 2D billboard prims occlude for real at
  // the deferred OT walk (gpu_native). Per-Core — was the process-globals g_pkt_track/lo/hi
  // (deglobalize-game 2026-07-02).
  int      mPktTrack = 0;
  uint32_t mPktLo    = 0xFFFFFFFFu;
  uint32_t mPktHi    = 0;

  // ---- render-side per-Core subsystems ------------------------------------
  NodeXform mNodeXform;   // scene-node WORLD-TRANSFORM builder (guest FUN_80051844)

  // ---- per-frame render orchestrators (called by Engine::fieldFrame/X) ----
  // frame  (guest 0x8003F9A8) — the primary per-frame render orchestrator; runs the non-walk PSX
  //   passes (the walk cluster is owned by SceneNative::render in engine_render_walk.cpp).
  //   Was ov_render_frame in engine_render.cpp.
  // frameX (guest 0x8003FA44) — the mid-transition variant (reduced pass set). Was ov_render_frame_x.
  void frame();
  void frameX();

  // perObjFlush: per-object native GT3/GT4 flush — composes the float camera×object transform from
  // the object's real world coords and submits every geomblk cmd on node+0xC0 through native_gt3gt4.
  // Taxi-parameter c->r[4] = node (recomp-shaped body, mirrors the guest ABI).
  void perObjFlush();

  // perObjRender: per-object render dispatch (guest 0x8003CCA4). Stashes current-object bookkeeping,
  // picks a per-node dispatch case, routes flush-only via perObjFlush and secondary-effect cases via
  // rec_super_call, then tags the produced packet span with the object's world-position depth.
  // Taxi-parameter c->r[4] = node. Was ov_perobj_render / submit_perobj_render.
  void perObjRender();

  // bgRender: field seaside GROUND/BG node renderer — overlay 0x8013E9D8 native. Runs the GTE
  // visibility/bound setup (rec_dispatch), then routes to submit_perobj_render for the native
  // world-coord render. Taxi-parameter c->r[4] = node. Was ov_bg_render.
  void bgRender();

  // Aux render-list WALKS — the three per-frame auxiliary render lists the master scene render walks
  // after the primary walk. Each drains a snapshot-double-buffered queue and dispatches each live node
  // through its per-type case. Recomp-shaped bodies. Were ov_rwalk_aux_bcf4/bf00/eec0.
  void rwalkAuxBcf4();
  void rwalkAuxBf00();
  void rwalkAuxEec0();

  // renderWalk: the master phase-2 render-list WALK (gen_func_8003C048). Drains the primary render
  // list, dispatching each live node by render type through the 33-entry jump table. Was ov_render_walk.
  void renderWalk();

  // renderWalkSnapshot: the SNAPSHOT-QUEUE object render driver (gen_func_8003BB50). Drains the
  // per-object render QUEUE (scratchpad cursor), tagging each object's packet span with its world
  // depth. Was ov_render_walk_snapshot.
  void renderWalkSnapshot();

  // terrain (guest 0x8002AB5C): the field terrain render entry. Picks the area's light config for
  // the frame, then either super-calls the recomp body (dual-core diff neutralize path) or runs the
  // PC-native float terrain render. Taxi-parameter c->r[4] = node. Was ov_terrain.
  void terrain();

  // rwalkB588 (guest 0x8003B588): the field WATER render pass — later-231 "Pass A". Owns the water
  // node 0x800E7E80's byte-state bookkeeping natively, runs the PSX per-object transform SETUP leaf
  // (rec_dispatch), then routes the render to the native submit_perobj_render for real-depth world-
  // coord projection. Was ov_rwalk_b588.
  void rwalkB588();

  // sceneNative: the master scene-render walker (per-frame terrain + entity/object walk over
  // the 3 doubly-linked object lists, with backdrop + collectable-quad + native BG tilemap).
  // Called by game_tomba2.cpp's ov_draw_otag every field-stage frame (and by margin_render's
  // widescreen re-include pass). Was ov_scene_native.
  void sceneNative();

  // fieldEntityRender: world-space GT3/GT4 scene-table renderer. Walks the entity-list struct at
  // `es` (per-list ptr headers at es+0x10..; packed geometry base at es+0xC; count at es+6),
  // dispatches each entry through submit_poly_gt3_native / submit_poly_gt4_native under the
  // camera-composed eproj xform. Called for the ground scene table 0x800F2418 (in Render::frame
  // groundnative diag branch and in the field object walk in engine_render_walk). Was
  // ov_field_entity_render (taxi-parameter c->r[4]).
  void fieldEntityRender(uint32_t es);
};
