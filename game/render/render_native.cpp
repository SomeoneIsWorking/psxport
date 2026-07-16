// game/render/render_native.cpp — NativeScenePass orchestrator.
//
// The START of the real PC-game render subsystem. ONE pass, fully decoupled from the PSX path:
//   1) NativeScenePass::collect (scene_build.cpp): walk the engine's entity lists + camera -> the
//      per-instance RenderScene draw list of float-transformed mesh objects (native intermediate rep).
//   2) NativeScenePass::drawObject (mesh_draw.cpp): for each object, parse its geomblk, transform
//      verts in FLOAT, project, and draw textured tris/quads into the REAL depth buffer.
//
// No GTE, no OT, no GP0 packet. The PSX-vanilla render path stays entirely separate and intact; this is
// an additive pass invoked only when the `rendernative` diagnostic channel is enabled (see game_tomba2.cpp
// ov_draw_otag). When the channel is off, none of this runs.
#include "core.h"
#include "cfg.h"
#include "render_native.h"
#include <stdio.h>

void NativeScenePass::run() {
  collect();

  int prims = 0;
  for (int i = 0; i < mScene.count; i++)
    prims += drawObject(&mScene.obj[i], &mScene.cam);

  if (cfg_dbg("rendernative")) {
    static long fr = 0;
    if ((fr++ % 60) == 0)
      cfg_logf("rendernative", "drew %d objects, %d prims (native decoupled pass)",
               mScene.count, prims);
  }
}
