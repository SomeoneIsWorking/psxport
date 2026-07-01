// engine/collision.h — PC-native COLLISION-GRID subsystem.
// The collision-grid family: row-pointer setup, cell query / neighbor-walk, resolve loop, per-step
// origin/index setup, and the list-tail resolver. Extracted from game_tomba2.cpp into its own module
// (PC-game code structure); registered in game_tomba2.cpp's init block by these names.
#ifndef ENGINE_COLLISION_H
#define ENGINE_COLLISION_H
struct Core;
void ov_list_scan_31780(Core* c);     // FUN_80031780 — list-tail resolver / reset
void ov_grid_setup_49968(Core* c);    // FUN_80049968 — collision-grid row-pointer setup
void ov_grid_query_47cbc(Core* c);    // FUN_80047CBC — collision-grid cell query / neighbor-walk
void ov_grid_resolve_498c8(Core* c);  // FUN_800498C8 — collision-grid resolve loop
void ov_grid_step_4798c(Core* c);     // FUN_8004798C — collision-grid per-step origin/index setup
#endif
