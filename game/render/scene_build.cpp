// game/render/scene_build.cpp — NATIVE scene enumeration: engine state -> RenderScene draw list.
//
// Walks the engine's three active entity lists, and for every LIVE 3D-MESH node builds a float
// model->view transform from the node's OWN world data (euler rotation + position + scale) composed with
// the scene camera, and records each render-command geomblk as a SceneObject. NO PSX intricacies: no OT,
// no GP0, no GTE op — just the engine's scene data read as floats.
//
// Entity model (from the object subsystem, game/world/, and memory [[object-pipeline-and-depth-regression]]):
//   - 3 list heads: 0x800FB168 / 0x800F2624 / 0x800F2738; next ptr @node+0x24 (0 = end).
//   - live flag @node+1 (0 = dead/free slot).
//   - render-intrinsic @node+0xb: 0x0F = 3D mesh (we draw these), 0x10..0x14 = 2D billboard (sprite path,
//     not handled in this first cut), 0x20 = off-world (skip).
//   - render-command array @node+0xC0, count @node+8; per cmd: geomblk = mem_r32(cmd+0x40).
//   - world transform: euler rot s16 @node+0x54/0x56/0x58 ; position s16 @node+0x2e/0x32/0x36 ;
//     scale @node+0xb8/0xba/0xbc (0x1000 = 1.0).
//
// Camera: the scene camera VIEW matrix lives in the scratchpad at draw time — rotation @0x1F8000F8
// (CR0-4 halfword packing, /4096) and translation @0x1F80010C/110/114 (int32 view units). This mirrors
// what native_terrain.cpp reads; it is the engine's own composed camera, not a PSX render artifact.
#include "core.h"
#include "cfg.h"
#include "render_native.h"
#include "scene_data.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// CR24/25/26 (projection center + H). These are the camera projection the engine configured (libgte
// SetGeomOffset/SetGeomScreen, owned in game_tomba2.cpp). A future engine-owned camera supplies them
// directly; for now we read them like the rest of the native projection path (projection.cpp).
uint32_t gte_read_ctrl(uint32_t reg);

#define SCR 0x1F800000u

static inline float r16f(Core* c, uint32_t a) { return (float)c->mem_r16s(a); }

// Build a 3x3 rotation from ZYX euler angles given as PSX fixed-point (4096 = 2*pi / 0x1000 ticks =
// one full turn). The game stores angles as s16 where 0x1000 == 360 deg. We convert to radians and
// compose Rz*Ry*Rx — the conventional model-orientation order. (This is the float, engine-owned
// orientation build; no GTE RotMatrix.)
static void euler_to_R(float ax, float ay, float az, float R[3][3]) {
  const float TWO_PI = 6.28318530718f;
  float rx = ax * (TWO_PI / 4096.0f);
  float ry = ay * (TWO_PI / 4096.0f);
  float rz = az * (TWO_PI / 4096.0f);
  float cx = cosf(rx), sx = sinf(rx);
  float cy = cosf(ry), sy = sinf(ry);
  float cz = cosf(rz), sz = sinf(rz);
  // Rz * Ry * Rx
  R[0][0] = cz*cy;             R[0][1] = cz*sy*sx - sz*cx;  R[0][2] = cz*sy*cx + sz*sx;
  R[1][0] = sz*cy;             R[1][1] = sz*sy*sx + cz*cx;  R[1][2] = sz*sy*cx - cz*sx;
  R[2][0] = -sy;               R[2][1] = cy*sx;             R[2][2] = cy*cx;
}

// Read the scene camera (view rotation Rcam + translation Tcam) from the scratchpad as floats.
static void read_camera(Core* c, float Rcam[3][3], float Tcam[3]) {
  uint32_t k0 = c->mem_r32(SCR+0xF8), k1 = c->mem_r32(SCR+0xFC), k2 = c->mem_r32(SCR+0x100),
           k3 = c->mem_r32(SCR+0x104), k4 = c->mem_r32(SCR+0x108);
  Rcam[0][0]=(int16_t)k0/4096.0f;        Rcam[0][1]=(int16_t)(k0>>16)/4096.0f; Rcam[0][2]=(int16_t)k1/4096.0f;
  Rcam[1][0]=(int16_t)(k1>>16)/4096.0f;  Rcam[1][1]=(int16_t)k2/4096.0f;       Rcam[1][2]=(int16_t)(k2>>16)/4096.0f;
  Rcam[2][0]=(int16_t)k3/4096.0f;        Rcam[2][1]=(int16_t)(k3>>16)/4096.0f; Rcam[2][2]=(int16_t)k4/4096.0f;
  Tcam[0]=(float)(int32_t)c->mem_r32(SCR+0x10C);
  Tcam[1]=(float)(int32_t)c->mem_r32(SCR+0x110);
  Tcam[2]=(float)(int32_t)c->mem_r32(SCR+0x114);
}

int NativeScenePass::collect() {
  Core* c = mCore;
  RenderScene* out = &mScene;
  out->count = 0;

  float Rcam[3][3], Tcam[3];
  read_camera(c, Rcam, Tcam);
  out->cam.ofx = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;   // CR24 already carries the widescreen center
  out->cam.ofy = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
  out->cam.H   = (float)(uint16_t)gte_read_ctrl(26);

  int dbg = cfg_dbg("rendernative");
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  int nlive = 0, nmesh = 0, ncmd_total = 0;

  for (int li = 0; li < 3; li++) {
    for (uint32_t n = c->mem_r32(HEADS[li]), g = 0;
         n >= 0x80000000u && n < 0x80200000u && g < 1024;
         n = c->mem_r32(n + 0x24), g++) {
      if (c->mem_r8(n + 1) == 0) continue;                 // not live
      nlive++;
      uint8_t ri = c->mem_r8(n + 0xB);
      // 3D-MESH selection: a node renders as a geomblk MESH unless it is a 2D billboard (0x10..0x14) or
      // off-world (0x20). Empirically the field's static props are ri=0x00/0x02, the player/dynamic meshes
      // ri=0x0F — all of these have cmds + a valid geomblk (verified via the `ents` REPL tool). Billboards
      // (sprite path, not built here) and off-world nodes carry no mesh, so skip them.
      if ((ri >= 0x10 && ri <= 0x14) || ri == 0x20) continue;
      if (c->mem_r8(n + 8) == 0) continue;                 // no render commands -> no mesh
      nmesh++;

      // model orientation from node euler (node+0x54/0x56/0x58)
      float ax = r16f(c, n + 0x54), ay = r16f(c, n + 0x56), az = r16f(c, n + 0x58);
      float Rmodel[3][3]; euler_to_R(ax, ay, az, Rmodel);
      // scale (node+0xb8/0xba/0xbc; 0x1000 = 1.0)
      float sx = (float)c->mem_r16s(n + 0xB8) / 4096.0f;
      float sy = (float)c->mem_r16s(n + 0xBA) / 4096.0f;
      float sz = (float)c->mem_r16s(n + 0xBC) / 4096.0f;
      if (sx == 0.0f && sy == 0.0f && sz == 0.0f) { sx = sy = sz = 1.0f; }   // unset scale -> identity
      float Rms[3][3];
      for (int i = 0; i < 3; i++) { Rms[i][0]=Rmodel[i][0]*sx; Rms[i][1]=Rmodel[i][1]*sy; Rms[i][2]=Rmodel[i][2]*sz; }
      // world position (node+0x2e/0x32/0x36, s16)
      float P[3] = { r16f(c, n + 0x2E), r16f(c, n + 0x32), r16f(c, n + 0x36) };

      // compose model->view: Rview = Rcam * Rms ; Tview = Rcam * P + Tcam
      float Rview[3][3], Tview[3];
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          float s = 0; for (int k = 0; k < 3; k++) s += Rcam[i][k] * Rms[k][j];
          Rview[i][j] = s;
        }
        float t = Tcam[i]; for (int k = 0; k < 3; k++) t += Rcam[i][k] * P[k];
        Tview[i] = t;
      }

      // one SceneObject per render-command geomblk (a node's full model = all its cmd geomblks)
      int ncmd = c->mem_r8(n + 8);
      for (int i = 0; i < ncmd && out->count < SCENE_MAX_OBJECTS; i++) {
        uint32_t cmd = c->mem_r32(n + 0xC0 + (uint32_t)i * 4);
        if (cmd < 0x80000000u || cmd >= 0x80200000u) continue;
        uint32_t geomblk = c->mem_r32(cmd + 0x40);
        if (geomblk < 0x80000000u || geomblk >= 0x80200000u) continue;
        SceneObject* o = &out->obj[out->count++];
        o->geomblk = geomblk;
        o->node = n;
        for (int r = 0; r < 3; r++) { o->R[r][0]=Rview[r][0]; o->R[r][1]=Rview[r][1]; o->R[r][2]=Rview[r][2]; o->T[r]=Tview[r]; }
        ncmd_total++;
      }
    }
  }

  if (dbg) {
    if ((mDbgFrame++ % 60) == 0)
      cfg_logf("rendernative", "scene: %d live, %d 3D-mesh nodes, %d geomblk objects (H=%.0f ofx=%.0f)",
               nlive, nmesh, out->count, (double)out->cam.H, (double)out->cam.ofx);
  }
  return out->count;
}
