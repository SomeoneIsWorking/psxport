// game/world/graphics_bind.h — PC-native OBJECT RENDER-BIND subsystem.
// Per-object display-record allocation + init, geometry/transform-block attach, and position-compose +
// render-state refresh: the bridge between a world entity and the data the renderer consumes. The recomp
// bodies stay the reference/super-call; the matrix/render primitives they call stay reachable by address.
//
// ov_build_xform (FUN_80051C8C, node+0x98 matrix) still in engine/engine_submit.cpp pending render-subsystem migration.
#ifndef GAME_WORLD_GRAPHICS_BIND_H
#define GAME_WORLD_GRAPHICS_BIND_H
struct Core;
void ov_record_alloc_g(Core* c);     // FUN_8007AAE8 — render-record bump allocator
void ov_obj_record_init(Core* c);    // FUN_80051B70 — per-object render-record init
void ov_obj_render_update(Core* c);  // FUN_800517F8 — per-object render-state update
void ov_obj_set_geom(Core* c);       // FUN_80077B38 — set object's geometry-block ptr from a table
void ov_obj_set_xformblk(Core* c);   // FUN_8006CBD0 — copy transform block into scratchpad + obj
void ov_obj_pos_compose(Core* c);    // FUN_8004BD64 — position-compose + render-state refresh
#endif
