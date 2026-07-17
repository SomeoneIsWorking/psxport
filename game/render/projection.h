// game/render/projection.h — PC-NATIVE object render transform & projection.
//
// A real PC game keeps its objects and camera as WORLD-SPACE data and transforms vertices in float; it
// does not consult the PSX GTE to decide where a vertex lands or how deep it is. The projection data types
// live here; the operations are methods on Render (see render.h) — projComposeObject / projComposeCamera
// set up an EObjXform from the engine's world state, and projSetActive / projVertexActive / projActiveCr /
// projClearActive own the per-command active-xform used by the GT3/GT4 submitters. EObjXform::project runs
// the per-vertex RTPT (rotate/translate + perspective divide) in float — a pure op on the transform, no
// Core state.
#pragma once
#include <stdint.h>
#include "proj_vtx.h"   // ProjVtx (framework POD) — game consumers still get it via projection.h

// A PC-native object render transform: composed camera × object, in float. R is the composed rotation in
// 1.3.12 scale (≈±4096, the units the RTPT expects), T the composed view translation, and ofx/ofy/H the
// camera's float projection constants (screen center + projection-plane distance).
struct EObjXform {
  float R[3][3];
  float T[3];
  float ofx, ofy, H;

  // Project model vertex V through this transform to float screen + view depth (full RTPT in float). No GTE.
  void project(int vx, int vy, int vz, ProjVtx* out) const;
};
