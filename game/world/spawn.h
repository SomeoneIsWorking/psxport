// class Spawn — PC-native ENTITY SPAWN / DESPAWN subsystem, owned by Engine.
//
// PROPER OOP: one instance per Core, reached as `c->engine.spawn.method(args)`. Back-pointer
// `core` wired once at Core construction time (same pattern as Placement / Animation / Collision).
//
// SCOPE: the engine path that instantiates and retires objects in the active object pool — pool-pop +
// field-init + list-link on spawn, and the mirror list-unlink + free-list return on despawn. Covers:
//   FUN_80079C3C  spawn primitive         (pool-pop + list-link)
//   FUN_8007A980  per-type dispatcher
//   FUN_8007A624  despawn primitive
//   FUN_8003116C  spawn-and-init helper
// The per-type spawn dispatch tables (0x80016e4c/0x80016e64) and their thin handlers stay PSX
// (content routing). Was the free functions `spawn_dispatch` / `world_despawn` / `world_spawn_and_init`
// taking Core*; the class removes that surface — Core is reached via the back-pointer.
#ifndef GAME_WORLD_SPAWN_H
#define GAME_WORLD_SPAWN_H
#include <cstdint>
struct Core;

class Spawn {
public:
  Core* core = nullptr;

  // dispatch(cls, type, list): FUN_8007A980 per-type spawn dispatcher. cls picks the per-type spawn
  //   VARIANT; type and list are the object's type byte + destination linked-list. Returns node ptr
  //   (0 on out-of-range cls, or if the pool is empty).
  uint32_t dispatch(uint32_t cls, uint32_t type, uint32_t list);

  // despawn(node): FUN_8007A624 — unlink node from its live list and return it to the free pool. The
  //   live-wiring entry the per-object AI behavior handlers use in place of `rec_dispatch(c,
  //   0x8007A624u)` where their control flow is already native (PC calls PC for what it owns).
  void despawn(uint32_t node);

  // spawnAndInit(a0, posSrc, a2): FUN_8003116C — pool-0 spawn (cls=0/type=6/list=1) + optional
  //   position seed from `posSrc` + heading `a2`, then a PSX per-object init (FUN_80028E10) run via
  //   rec_dispatch. Returns node ptr (0 on pool-empty).
  uint32_t spawnAndInit(uint32_t a0, uint32_t posSrc, uint32_t a2);

  // sceneEntity(sceneId, subtype): FUN_8007E110 — allocate a SCENE-ENTITY node (class-3 tail-insert
  //   into active list 1 via the FUN_8007A5A8 allocator), install the per-frame scene-entity handler
  //   (0x8007DDE0), and initialise the node's data-table pointers from *(u32)0x800ECF60 (the field
  //   scene-entity data table). Returns node ptr on success or 0 on pool exhaustion (the caller
  //   stashes the return in Actor::sceneHandle (obj+0x14) and treats nonzero as "spawned"). The
  //   per-frame handler FUN_8007DDE0 stays reachable via its stored address (recomp substrate).
  uint32_t sceneEntity(uint16_t sceneId, uint8_t subtype);
};

#endif
