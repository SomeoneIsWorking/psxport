// game/world/placement.h — PC-native field OBJECT-PLACEMENT subsystem.
// The field/area object-placement driver (FUN_80072A78) + the single-object spawn-with-parent helper
// (FUN_80072DDC): read the active area's placement TABLE and populate the field with its NPCs/items/
// scenery via the owned spawn dispatcher. The recomp bodies stay the reference/super-call.
#ifndef GAME_WORLD_PLACEMENT_H
#define GAME_WORLD_PLACEMENT_H
struct Core;
void ov_place_objects(Core* c);        // FUN_80072A78 — field object-placement driver
void ov_spawn_with_parent(Core* c);    // FUN_80072DDC — single-object spawn-with-parent helper
#endif
