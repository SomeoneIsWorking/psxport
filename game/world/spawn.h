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
// FUN_8007A980 — per-type spawn dispatcher. Was `c->r[4] = cls; c->r[5] = type; c->r[6] = list;
// spawn_dispatch(c); c->r[2] gets node` (taxi). Now explicit typed args; returns node ptr (0 on
// out-of-range class). Callers: placement.cpp / sop.cpp.
uint32_t spawn_dispatch(Core* c, uint32_t cls, uint32_t type, uint32_t list);
void entity_spawn_register(void);
// world_despawn(c, node) — FUN_8007A624, the DESPAWN primitive (oracle-verified 0-diff, see ov_despawn in
// spawn.cpp). Typed live-wiring entry: the per-object AI behavior handlers (game/ai/beh_*.cpp) call this
// directly in place of `rec_dispatch(c, 0x8007A624u)` wherever their own control flow is already native —
// PC calls PC for what it owns, instead of round-tripping through the substrate for an owned leaf.
void world_despawn(Core* c, uint32_t node);
// world_spawn_and_init(c, a0, posSrc, a2) — FUN_8003116C, the SPAWN-AND-INIT helper (oracle-verified via
// spawninitverify, see ov_spawn_and_init). Same live-wiring role as world_despawn, for spawn call sites.
uint32_t world_spawn_and_init(Core* c, uint32_t a0, uint32_t posSrc, uint32_t a2);
#endif
