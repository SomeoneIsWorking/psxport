// game/world/spawn.h — PC-native ENTITY SPAWN subsystem.
// The engine path that instantiates a new game object into the active object pool: pop a node from the
// shared free-list, initialize its identity fields (active byte / type / list-id), and link it into one
// of the three live doubly-linked object lists at the requested position. The recomp body stays the
// reference/super-call; the per-type spawn dispatch tables (0x80016e4c/0x80016e64) and their thin
// handlers stay PSX (content routing). Registered from game_tomba2.cpp by entity_spawn_register().
#ifndef GAME_WORLD_SPAWN_H
#define GAME_WORLD_SPAWN_H
struct Core;
void ov_entity_spawn(Core* c);   // FUN_80079C3C — pool-pop + field-init + list-link spawn primitive
void spawn_dispatch(Core*);      // FUN_8007A980 — per-type spawn dispatcher (used by placement.cpp)
void entity_spawn_register(void);
#endif
