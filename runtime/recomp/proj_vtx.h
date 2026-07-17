// runtime/recomp/proj_vtx.h — ProjVtx: the projected-vertex POD (framework-side).
//
// Extracted from game/render/projection.h so the framework (gte_beetle.cpp) can name the projection
// output type without pulling in the game's per-object transform (EObjXform). Pure POD, no game types,
// no Core: float screen px/py + view depth pz, plus the integer SX/SY/SZ and view-space IR the
// rasterizer/cull/lighting consume. game/render/projection.h #includes this so game consumers still
// get ProjVtx via projection.h as before.
#pragma once

typedef struct { int ir1, ir2, ir3, sz, sx, sy; float px, py, pz, vx, vy, vz; } ProjVtx;
