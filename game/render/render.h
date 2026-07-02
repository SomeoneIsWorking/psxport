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

  // ---- render-side per-Core subsystems ------------------------------------
  NodeXform mNodeXform;   // scene-node WORLD-TRANSFORM builder (guest FUN_80051844)

  // ---- per-frame render orchestrators (called by Engine::fieldFrame/X) ----
  // frame  (guest 0x8003F9A8) — the primary per-frame render orchestrator; runs the non-walk PSX
  //   passes (the walk cluster is owned by SceneNative::render in engine_render_walk.cpp).
  //   Was ov_render_frame in engine_render.cpp.
  // frameX (guest 0x8003FA44) — the mid-transition variant (reduced pass set). Was ov_render_frame_x.
  void frame();
  void frameX();

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
