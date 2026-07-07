// game/render/render_native.h — the NATIVE render pass subsystem.
//
// class NativeScenePass builds the frame from native SCENE DATA (entity lists -> RenderScene) and draws
// every 3D-mesh object with float transforms + real depth, FULLY DECOUPLED from the PSX render path (no
// OT, no GP0, no GTE op). It is an ADDITIVE pass: the existing PSX-vanilla render path is untouched. The
// invocation (Render::mNativeScene.run) is gated behind the `rendernative` diagnostic channel until this
// becomes the default.
#ifndef GAME_RENDER_NATIVE_H
#define GAME_RENDER_NATIVE_H
#include "scene_data.h"

class Core;

class NativeScenePass {
public:
  Core* mCore = nullptr;

  // Orchestrate one native-decoupled render pass: collect() -> drawObject() per scene entry.
  void run();

  // Walk the engine's scene state (three entity lists + camera) and populate mScene. Reads guest RAM
  // only; writes nothing. Returns the object count. (impl: scene_build.cpp)
  int collect();

  // Draw one SceneObject's geomblk: transform each GT3/GT4 prim's model verts by the object's float
  // model->view matrix, project with the camera, and submit as a textured quad/tri with real depth.
  // Returns the number of prims drawn. (impl: mesh/mesh_draw.cpp)
  int drawObject(const SceneObject* o, const SceneCamera* cam);

  // The PC-native float terrain render pass (guest 0x8002AB5C's render half). Taxi-parameter
  // mCore->r[4] = the terrain render-list node. (impl: native_terrain.cpp)
  void terrainRender();

private:
  // Large per-frame draw list (768 SceneObjects) — kept on the class instance so it isn't a file-scope
  // static (per [[deglobalize_game_and_runtime_oop]]) and isn't on the stack. One buffer per Core; SBS's
  // two cores each get their own.
  RenderScene mScene;
  // `rendernative` debug-log frame counter (collect()'s once-per-60 stats line).
  long mDbgFrame = 0;
};

#endif
