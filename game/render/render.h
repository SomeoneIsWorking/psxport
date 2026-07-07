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
#include "render_mode.h"
#include "render_diag.h"
#include "pkt_span.h"
#include "dualview_snapshot.h"
#include "render_stats.h"
#include "proj_prim.h"
#include "pgxp.h"
#include "proj_params.h"
#include "engine_project.h"     // EObjXform (per-Core active per-object xform lives on Render below)
#include "render_native.h"      // class NativeScenePass — the decoupled native render subsystem
#include "margin_render.hpp"    // class MarginRenderer — widescreen margin collect-and-flush
#include "lighting.h"           // class Lighting — per-area light registry (sun / lava+torch)
class Core;

class Render {
public:
  Core* mCore = nullptr;

  // ---- render-side per-Core subsystems ------------------------------------
  RenderMode        mode;              // compare-mode toggles (psxRender / dualview)
  RenderDiag        diag;              // per-object walk-scope tags (currentNode, currentGeomblk)
  PktSpan           pktSpan;           // packet-pool store-address-span tracker (Core::mem_w* -> track)
  DualviewSnapshot  dualviewSnapshot;  // dual-view render harness's per-Core RAM+scratchpad+GTE snapshots
  RenderStats       stats;             // per-frame render diag counters (ndepth / obj-depth / projprim)
  ProjPrim          projprim;          // vertex-depth cache for native depth path (per-Core; SBS-safe)
  Pgxp              pgxp;               // PGXP-lite subpixel cache (per-Core; PGXP_pushSXYZ2f target)
  ProjParams        projParams;         // camview + per-frame projection constants (per-Core)
  // Active per-object xform for the GT3/GT4 submitters. Set once per render command by the per-object
  // flush (projSetActive), read by the per-vertex projection (projVertexActive), cleared by
  // projClearActive. Was file-scope in engine_project.cpp; per-Core here so SBS's two cores don't
  // share a transform between their emits (2026-07-03).
  EObjXform         mActiveXform{};
  bool              mActiveXformSet = false;
  NodeXform         mNodeXform;        // scene-node WORLD-TRANSFORM builder (guest FUN_80051844)
  // PSXPORT_BDTAG per-node attribution names for the walk dispatch cases (engine_render_walk.cpp).
  // The gp0 classify is deferred one frame, so the names live in a per-Core ring, not walk locals.
  char              mWalkTagRing[512][20] = {};
  int               mWalkTagPos = 0;
  NativeScenePass   mNativeScene;      // decoupled native render pass (collect + drawObject)
  MarginRenderer    margin;            // widescreen margin re-include (collect in cull, flush post-walk)
  Lighting          lighting;          // per-area light registry (selected once per frame by shadeSelect)
  // Light config selected for this frame by shadeSelect(); the hot per-face shading routine reads the
  // cached pointer instead of re-reading guest RAM. Falls back to the SUN default when unset.
  const LightConfig* mShadeCfg = nullptr;

  // ---- object-render projection ops (impl in engine_project.cpp) ----------
  // Compose an EObjXform from the object's REAL WORLD coordinates: its world rotation matrix (cmd+0x18)
  // and world position (cmd+0x2C), transformed by the live scene camera (scratchpad view matrix
  // 0x1F8000F8 / translation 0x1F80010C). Projection constants are the camera's (CR24-26). No gte_op.
  void projComposeObject(uint32_t cmd, EObjXform* out);
  // Compose an EObjXform from the scene camera ALONE (no per-object matrix) — for geometry already in
  // WORLD space (field entity render loop), where view = Rcam·world + Tcam directly. No gte_op.
  void projComposeCamera(EObjXform* out);
  // Per-command active xform (owned by the GT3/GT4 submitters).
  void projSetActive(const EObjXform* w);
  void projClearActive();
  bool projActive() const { return mActiveXformSet; }
  void projVertexActive(int vx, int vy, int vz, ProjVtx* out);
  // Pack the ACTIVE float xform into the CR0-7 + CR24/25/26 layout the fps60 midpoint reprojection consumes.
  void projActiveCr(uint32_t cr[11]);

  // ---- per-frame render orchestrators (called by Engine::fieldFrame/X) ----
  // frame  (guest 0x8003F9A8) — the primary per-frame render orchestrator; runs the non-walk PSX
  //   passes (the walk cluster is owned by SceneNative::render in engine_render_walk.cpp).
  //   Was ov_render_frame in engine_render.cpp.
  // frameX (guest 0x8003FA44) — the mid-transition variant (reduced pass set). Was ov_render_frame_x.
  void frame();
  void frameX();

  // perObjFlush: per-object native GT3/GT4 flush — composes the float camera×object transform from
  // the object's real world coords and submits every geomblk cmd on node+0xC0 through gt3gt4.
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
  // walkTag: format one BDTAG attribution name ("<pfx><fn low20>") into mWalkTagRing.
  const char* walkTag(const char* pfx, uint32_t fn);

  // renderWalkSnapshot: the SNAPSHOT-QUEUE object render driver (gen_func_8003BB50). Drains the
  // per-object render QUEUE (scratchpad cursor), tagging each object's packet span with its world
  // depth. Was ov_render_walk_snapshot.
  void renderWalkSnapshot();

  // terrain (guest 0x8002AB5C): the field terrain render entry. Picks the area's light config for
  // the frame, then either super-calls the recomp body (dual-core diff neutralize path) or runs the
  // PC-native float terrain render. Taxi-parameter c->r[4] = node. Was ov_terrain.
  void terrain();

  // shadeSelect: pick this area's light config once per world frame (cheap guest-RAM fingerprint via
  // Lighting::areaKeyFrom); caches the result in mShadeCfg for the per-face shading routine.
  void shadeSelect();

  // gt3gt4 (gen_func_800803DC's first body): the generic GT3/GT4 renderer — split the geomblk's packed
  // prim counts (low16 tri, high16 quad) and run the two native submitters in sequence.
  void gt3gt4(uint32_t geomblk, uint32_t otbase);

  // prepObjectMatrix: shared terrain scene-data prep (the faithful gameplay half of guest 0x8002AB5C):
  // depth-cue regs + the two sway gameplay bytes, then the object rotation matrix at scratch SCR.
  void prepObjectMatrix(uint32_t node);

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

private:
  // Native POLY_GT3/GT4 submitters (guest-ABI bodies: rec/otbase/count in r4/r5/r6).
  static void submitPolyGt3Native(Core* c);   // gen_func_8007FDB0
  static void submitPolyGt4Native(Core* c);   // gen_func_80080114
};
