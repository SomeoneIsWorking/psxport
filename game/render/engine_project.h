// engine/engine_project.h — PC-NATIVE object render transform & projection.
//
// A real PC game keeps its objects and camera as WORLD-SPACE data and transforms vertices in float; it
// does not consult the PSX GTE to decide where a vertex lands or how deep it is. This module owns exactly
// that for the per-object render: it composes the camera × object transform from the object's REAL WORLD
// coordinates (world rotation matrix + world position) and the live scene camera, then projects each model
// vertex to float screen coords + view depth — with NO gte_op and NO read of the GTE-composed CR0-7. The
// per-object render path is fully decoupled from the PSX here.
#pragma once
#include <stdint.h>

struct Core;

// Projected vertex: float screen px/py, view depth pz, plus the integer SX/SY/SZ and view-space IR the
// rasterizer/cull/lighting consume. (Same layout the submitters already use.)
typedef struct { int ir1, ir2, ir3, sz, sx, sy; float px, py, pz, vx, vy, vz; int mx, my, mz; } ProjVtx;

// A PC-native object render transform: composed camera × object, in float. R is the composed rotation in
// 1.3.12 scale (≈±4096, the units the RTPT expects), T the composed view translation, and ofx/ofy/H the
// camera's float projection constants (screen center + projection-plane distance).
typedef struct { float R[3][3]; float T[3]; float ofx, ofy, H; } EObjXform;

// Compose `out` from the object's REAL WORLD coordinates: its world rotation matrix (cmd+0x18) and world
// position (cmd+0x2c), transformed by the live scene camera (scratchpad view matrix 0x1F8000F8 / translation
// 0x1F80010C). Projection constants are the camera's (CR24-26, set once per frame by the engine). No gte_op.
void eproj_compose_object(struct Core* c, uint32_t cmd, EObjXform* out);

// Compose `out` from the scene camera ALONE (no per-object matrix): R = camera view rotation, T = camera
// view translation, projection = CR24-26. For geometry whose vertices are already in WORLD space (the
// field's entity render loop), so view = Rcam·world + Tcam directly. No gte_op.
void eproj_compose_camera(struct Core* c, EObjXform* out);

// Project model vertex V through `w` to float screen + view depth (full RTPT in float). No GTE.
void eproj_vertex(const EObjXform* w, int vx, int vy, int vz, ProjVtx* out);

// The per-object submitters project against the ACTIVE object xform, set once per render command by the
// per-object flush. eproj_vertex_active is the submitters' projection entry — fully world-coord driven.
// State moved onto Render (mActiveXform / mActiveXformSet) so SBS's two cores keep separate active
// transforms; every entry takes Core* to reach THIS core's Render instance.
void eproj_set_active(struct Core* c, const EObjXform* w);
void eproj_clear_active(struct Core* c);
int  eproj_active(struct Core* c);
void eproj_vertex_active(struct Core* c, int vx, int vy, int vz, ProjVtx* out);

// Pack the ACTIVE float xform into the CR0-7 + CR24/25/26 layout the 60fps midpoint reprojection consumes
// (cr[0..7] = composed transform, cr[8]=OFX 16.16, cr[9]=OFY 16.16, cr[10]=H). Lets the fps60 mesh capture
// work off the world-coord transform instead of reading the GTE the native path no longer populates.
void eproj_active_cr(struct Core* c, uint32_t cr[11]);
