// class GraphicsBind — PC-native OBJECT RENDER-BIND subsystem.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::graphicsBind`. Back-pointer
// wired once by Core's constructor. Callers reach the bind cluster through the object graph:
//
//     c->engine.graphicsBind.recordAlloc();       // FUN_8007AAE8 — record bump allocator
//     c->engine.graphicsBind.recordInit();        // FUN_80051B70 — per-object record init
//     c->engine.graphicsBind.renderUpdate();      // FUN_800517F8 — per-object render-state update
//     c->engine.graphicsBind.setGeom();           // FUN_80077B38 — set geom-block ptr
//     c->engine.graphicsBind.setXformBlk();       // FUN_8006CBD0 — copy xform block to scratchpad
//     c->engine.graphicsBind.posCompose();        // FUN_8004BD64 — position-compose + refresh
//
// The bridge between a world entity and the data the renderer consumes. Underlying guest bodies
// stay as reference / super-call for the byte-A/B verify gates.
//
// ov_build_xform (FUN_80051C8C, node+0x98 matrix) still in engine/engine_submit.cpp pending
// render-subsystem migration.
#ifndef GAME_WORLD_GRAPHICS_BIND_CLASS_H
#define GAME_WORLD_GRAPHICS_BIND_CLASS_H
class Core;
class GraphicsBind {
public:
  Core* core = nullptr;
  void recordAlloc();     // FUN_8007AAE8
  void recordInit();      // FUN_80051B70
  void renderUpdate();    // FUN_800517F8
  void setGeom();         // FUN_80077B38
  void setXformBlk();     // FUN_8006CBD0
  void posCompose();      // FUN_8004BD64
};
#endif
