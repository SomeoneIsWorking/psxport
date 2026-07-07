// game/render/scene_data.h — the INTERMEDIATE REPRESENTATION of a frame for the NATIVE renderer.
//
// This is the data the decoupled native render path consumes. It is built by walking the engine's own
// scene state (the active entity lists + each node's graphics binding + the camera), NOT by reading any
// PSX render decision (no OT, no GP0 packet, no GTE output). Everything here is plain float / native ids.
//
// A `RenderScene` is the per-frame draw list: the camera (a float view + projection) plus a list of
// `SceneObject`s, each a model-space mesh (geomblk guest ptr) with a float model->world transform and a
// texture reference. The mesh sub-subsystem turns each SceneObject into float-projected textured prims.
#ifndef GAME_RENDER_SCENE_DATA_H
#define GAME_RENDER_SCENE_DATA_H
#include <stdint.h>

// One drawable mesh instance in the native scene. Matches game/world/graphics_bind.h's forward
// `SceneObject` in spirit (geometry ref + float transform + texture), expanded with the per-instance
// fields the mesh drawer needs. `world` is a column-major-ish 3x4 (rotation R[3][3] + translation T[3]).
struct SceneObject {
  uint32_t geomblk;     // model-space prim-list (GT3/GT4 records), guest ptr (mem_r* into RAM)
  float    R[3][3];     // model->view rotation (already composed with the camera; see scene_build)
  float    T[3];        // model->view translation (camera baked in)
  uint32_t node;        // source entity node (debug / attribution only)
};

// Float camera projection constants for the frame (screen center + projection-plane distance H).
struct SceneCamera {
  float ofx, ofy;       // screen-space projection center (CR24/CR25 in pixels)
  float H;              // projection-plane distance (CR26) — focal length
};

// A whole frame's worth of native draw data.
#define SCENE_MAX_OBJECTS 768
struct RenderScene {
  SceneCamera cam;
  SceneObject obj[SCENE_MAX_OBJECTS];
  int         count;
};

#endif
