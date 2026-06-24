// game/render/render_native.cpp — the NATIVE render pass orchestrator.
//
// The START of the real PC-game render subsystem. ONE pass, fully decoupled from the PSX path:
//   1) render_scene_collect (scene/scene_build.cpp): walk the engine's entity lists + camera -> a
//      RenderScene draw list of float-transformed mesh objects (the native intermediate representation).
//   2) mesh_draw_object (mesh/mesh_draw.cpp): for each object, parse its geomblk, transform verts in
//      FLOAT, project, and draw textured tris/quads into the REAL depth buffer (gpu_draw_world_quad).
//
// No GTE, no OT, no GP0 packet. The PSX-vanilla render path stays entirely separate and intact; this is
// an additive pass invoked only when the `rendernative` diagnostic channel is enabled (see game_tomba2.cpp
// ov_draw_otag). When the channel is off, none of this runs.
#include "core.h"
#include "cfg.h"
#include "scene/scene_data.h"
#include "mesh/mesh_draw.h"
#include "render_native.h"
#include <stdio.h>

void render_scene_native(Core* c) {
  static RenderScene scene;                 // large (768 SceneObjects) — keep off the stack
  render_scene_collect(c, &scene);

  int prims = 0;
  for (int i = 0; i < scene.count; i++)
    prims += mesh_draw_object(c, &scene.obj[i], &scene.cam);

  if (cfg_dbg("rendernative")) {
    static long fr = 0;
    if ((fr++ % 60) == 0)
      fprintf(stderr, "[rendernative] drew %d objects, %d prims (native decoupled pass)\n",
              scene.count, prims);
  }
}
