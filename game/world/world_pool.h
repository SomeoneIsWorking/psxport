// game/world/world_pool.h — shared internal types/constants for the PC-native WORLD OBJECT subsystem
// (the object pool + the three live doubly-linked object lists). The pool/list ADDRESS CONSTANTS and the
// `struct PoolDesc` (a free-list head + count pair) live here so the spawn/despawn TU has one home for
// them. Cross-TU shared SYMBOLS are declared in their owning headers, not here: spawn_dispatch in
// spawn.h (used by placement.cpp), the verify harness in game/core/verify_harness.h.
#ifndef GAME_WORLD_POOL_H
#define GAME_WORLD_POOL_H
#include <stdint.h>

// A pool free-list descriptor: the guest address of the free-list HEAD pointer (u32) and of the free
// COUNT byte (u8). Used by the spawn variants + the despawn free-push.
struct PoolDesc { uint32_t free_head, cnt; };

#endif
