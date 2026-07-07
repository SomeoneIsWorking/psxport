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
#ifndef GAME_WORLD_GRAPHICS_BIND_CLASS_H
#define GAME_WORLD_GRAPHICS_BIND_CLASS_H
class Core;
class GraphicsBind {
public:
  Core* core = nullptr;
  int mTrace = -1;   // PSXPORT_RECALLOC_TRACE latch (-1 = not read yet); recordAlloc caller-attribution
  void recordAlloc();     // FUN_8007AAE8
  void recordInit();      // FUN_80051B70
  void renderUpdate();    // FUN_800517F8
  void setGeom();         // FUN_80077B38
  void setXformBlk();     // FUN_8006CBD0
  void posCompose();      // FUN_8004BD64

  // installSceneRecord(rec, classArg, itemArg): FUN_80051B04 — two-level pointer resolve into the
  //   scene-data table at 0x800ECF58 (same table Spawn::sceneEntity reads at offset +8). Reads
  //   base = *(u32)(0x800ECF58 + classArg*4), then off = *(u32)(base + itemArg*4 + 4), stores
  //   (base + off) at rec[+0x40] — the sceneData pointer slot on a GraphicsBind render record.
  //   The recordInit body (FUN_80051B70) inlines the same three lines at its tail; this method is
  //   the shared source-of-truth. All 4 direct callsites pass classArg=12.
  void installSceneRecord(uint32_t rec, uint32_t classArg, uint32_t itemArg);

private:
  // Guest-ABI bodies (plain fn-pointer shape for the verify gate).
  static uint32_t recordAllocBody(Core* c);      // FUN_8007AAE8
  static uint32_t recordInitBody(Core* c);       // FUN_80051B70
  static uint32_t renderUpdateBody(Core* c);     // FUN_800517F8
  static uint32_t setGeomBody(Core* c);          // FUN_80077B38
  static uint32_t setXformBlkBody(Core* c);      // FUN_8006CBD0
  static uint32_t posComposeBody(Core* c);       // FUN_8004BD64
};
#endif
