// class GraphicsBind — PC-native OBJECT RENDER-BIND subsystem.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::graphicsBind`. Back-pointer
// wired once by Core's constructor. Callers reach the bind cluster through the object graph:
//
//     eng(c).graphicsBind.recordAlloc();       // FUN_8007AAE8 — record bump allocator
//     eng(c).graphicsBind.recordInit();        // FUN_80051B70 — per-object record init
//     eng(c).graphicsBind.renderUpdate();      // FUN_800517F8 — per-object render-state update
//     eng(c).graphicsBind.setGeom();           // FUN_80077B38 — set geom-block ptr
//     eng(c).graphicsBind.setXformBlk();       // FUN_8006CBD0 — copy xform block to scratchpad
//     eng(c).graphicsBind.posCompose();        // FUN_8004BD64 — position-compose + refresh
//
// The bridge between a world entity and the data the renderer consumes. Underlying guest bodies
// stay as reference / super-call for the byte-A/B verify gates.
#ifndef GAME_WORLD_GRAPHICS_BIND_CLASS_H
#define GAME_WORLD_GRAPHICS_BIND_CLASS_H
class Core;
class Game;
class GraphicsBind {
public:
  Core* core = nullptr;

  // registerOverrides — wires recordArrayInit into the override registry (overrides::install,
  // with shard_set_override so direct substrate callers redirect too). Unlike this class's OTHER
  // 6 methods (recordAlloc/recordInit/renderUpdate/setGeom/
  // setXformBlk/posCompose, wired via the verify-harness c->game->verify.run() A/B gate because
  // their callers have ALREADY been converted to direct native calls), recordArrayInit's callers
  // are still guest-ABI rec_dispatch(c, 0x800519E0u) sites in several un-ported AI beh_ handlers,
  // so it needs the standard dual-wire reach (same pattern as NodeXform::registerOverrides).
  static void registerOverrides(Game* game);
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

  // recordArrayInit(obj, count, sceneBase, tmpl): FUN_800519E0 — UNWIRED DRAFT (2026-07-08
  // wide-RE wave, region 0x80050000-0x8005FFFF). Batch sibling of recordInit(): allocates `count`
  // render records (bump allocator, same pool as recordInit) into obj[+0xC0 .. +0xC0+4*(count-1)],
  // seeding each from a 4-halfword template entry (tmpl[i*4 .. i*4+3] -> record+6/+0/+2/+4) and
  // resolving each record's sceneData pointer as `sceneBase + tmpl-array-driven-offset` (same
  // installSceneRecord idiom, but reading the offset from an ascending int32 array starting at
  // sceneBase+4 instead of the classArg/itemArg double-index). obj+8/+9 = count, obj+0xB8/BA/BC =
  // scale identity (0x1000), obj+0xD = 0. Returns 1 (obj despawn-pending, obj+9=0) if the record
  // pool doesn't have `count` slots free; else 0. See .cpp for the full RE + generated/shard_1.c
  // gen_func_800519E0 cross-check.
  uint32_t recordArrayInit(uint32_t obj, uint32_t count, uint32_t sceneBase, uint32_t tmpl);

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
