// game/render/mesh/mesh_draw.h — NATIVE mesh drawer: a geomblk + float transform -> projected textured
// tris/quads into the real depth buffer. No GTE, no OT, no GP0 packet — pure float transform + project +
// the engine's float world-quad draw helper (real per-pixel depth).
#ifndef GAME_RENDER_MESH_DRAW_H
#define GAME_RENDER_MESH_DRAW_H
#include <stdint.h>
struct Core;
struct SceneObject;
struct SceneCamera;

// Draw one SceneObject's geomblk: transform each GT3/GT4 prim's model verts by the object's float
// model->view matrix, project with the camera, and submit as a textured quad/tri with real depth.
// Returns the number of prims drawn. (game/render/mesh/mesh_draw.cpp)
int mesh_draw_object(Core* c, const SceneObject* o, const SceneCamera* cam);

#endif
