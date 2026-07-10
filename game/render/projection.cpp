// game/render/projection.cpp — PC-NATIVE object render transform & projection (see projection.h).
//
// The per-object render is decoupled from the PSX GTE here. Render::projComposeObject builds the camera ×
// object transform in FLOAT from the object's real world coordinates; EObjXform::project runs the RTPT
// (rotate/translate + perspective divide) in float. The math mirrors the engine's projection (gte_beetle
// proj_native_xform's float path) so per-object geometry lines up with terrain and the rest of the scene —
// but the rotation and translation come from world-space data, not the GTE-composed control registers.
//
// Compose, in world space (all int16 matrix elements are 1.3.12 fixed = value*4096):
//   Rcam = scene camera view rotation   (scratchpad 0x1F8000F8, CR0-4 packing)
//   Tcam = scene camera view translation (scratchpad 0x1F80010C/110/114)
//   Robj = object world rotation matrix  (cmd+0x18, columns at +0/+2/+4, rows at +0/+6/+0xC)
//   Tobj = object world position         (cmd+0x2C/0x30/0x34)
//   R = (Rcam · Robj) / 4096     (composed rotation, kept in 1.3.12 scale)
//   T = (Rcam · Tobj) / 4096 + Tcam   (composed view translation)
// This is exactly view = Rcam·(Robj·v + Tobj) + Tcam — a standard model→world→view transform.

#include "projection.h"
#include "render.h"
#include "core.h"
#include "game.h"    // c->game->fps60 — the TRUE per-object 60fps camera/object interpolation provider
#include "cfg.h"
#include <stdio.h>

// camera projection constants (screen center OFX/OFY, projection-plane H) — frame-constant, set by the
// engine's projection setup. Read from the GTE control regs (CR24-26): these are the camera's projection,
// not a per-object PSX compute. (A future engine-owned camera can supply them directly.)
uint32_t gte_read_ctrl(uint32_t reg);

#define SCR 0x1F800000u

static inline int16_t r16(Core* c, uint32_t a) { return c->mem_r16s(a); }

// Shared camera-compose core: R = (Rcam · Robj) / 4096, T = (Rcam · Tobj) / 4096 + Tcam, plus the
// camera's projection constants. Robj/Tobj are already-float object rotation/translation — either read
// from cmd+0x18/0x2C (projComposeObject) or host-computed by a caller whose object is stale in guest RAM
// (projComposeObjectHost). Camera state (Rcam/Tcam/ofx/ofy/H) is always read from scratchpad/GTE ctrl —
// the camera itself is never stale.
static void projComposeCore(Core* c, const float Robj[3][3], const float Tobj[3], EObjXform* out) {
  // Scene camera (Rcam int16 rows, Tcam raw view units, OFX/OFY/H) read through the fps60 provider. When
  // fps60 is off this is a plain read of the scratchpad view matrix + CR24-26 (byte-identical to the old
  // inline read); during the 60fps mid-present it returns the (prev,cur) MIDPOINT camera so the object
  // pans through the interpolated camera. It also CAPTURES this frame's camera host-side.
  float Rcam[3][3], Tcam[3], ofx, ofy, H;
  c->game->fps60.sceneCam(c, Rcam, Tcam, ofx, ofy, H);

  // composed rotation R = (Rcam · Robj) / 4096, kept in 1.3.12 scale.
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      double s = (double)Rcam[i][0] * Robj[0][j] + (double)Rcam[i][1] * Robj[1][j] + (double)Rcam[i][2] * Robj[2][j];
      out->R[i][j] = (float)(s / 4096.0);
    }
  // composed view translation T = (Rcam · Tobj) / 4096 + Tcam.
  for (int i = 0; i < 3; i++) {
    double s = (double)Rcam[i][0] * Tobj[0] + (double)Rcam[i][1] * Tobj[1] + (double)Rcam[i][2] * Tobj[2];
    out->T[i] = (float)(s / 4096.0) + Tcam[i];
  }
  out->ofx = ofx; out->ofy = ofy; out->H = H;
}

void Render::projComposeObject(uint32_t cmd, EObjXform* out) {
  Core* c = mCore;
  // object world rotation matrix Robj from cmd+0x18: Robj[row][col] = halfword at cmd+0x18 + col*2 + row*6.
  float Robj[3][3];
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++)
      Robj[row][col] = (float)r16(c, cmd + 0x18 + col * 2 + row * 6);
  // object world position Tobj from cmd+0x2C/0x30/0x34.
  float Tobj[3] = { (float)r16(c, cmd + 0x2C), (float)r16(c, cmd + 0x30), (float)r16(c, cmd + 0x34) };

  // fps60 TRUE per-object tier: capture this object's world transform (keyed by cmd) and, during the 60fps
  // mid-present, replace Robj/Tobj with the (prev,cur) midpoint so the object tweens through the real
  // projection. No-op / no capture when fps60 is off — Robj/Tobj pass through unchanged.
  c->game->fps60.objXform(cmd, Robj, Tobj);
  projComposeCore(c, Robj, Tobj, out);
  if (cfg_dbg("eproj")) { static long n = 0; if (n++ % 240 == 0)
    fprintf(stderr, "[eproj] native compose #%ld cmd=%08x Tview=(%.0f,%.0f,%.0f) H=%.0f\n",
            n, cmd, (double)out->T[0], (double)out->T[1], (double)out->T[2], (double)out->H); }
}

void Render::projComposeObjectHost(const float Robj[3][3], const float Tobj[3], EObjXform* out) {
  projComposeCore(mCore, Robj, Tobj, out);
}

void Render::projComposeCamera(EObjXform* out) {
  Core* c = mCore;
  // The field's entity (scene-table) verts are already world-space, so view = Rcam·world + Tcam and the
  // composed rotation IS the camera rotation. Read it through the fps60 provider (Rcam int16 rows) so the
  // scene table pans through the interpolated camera during the 60fps mid-present. Identical to the old
  // inline scratchpad read when fps60 is off.
  c->game->fps60.sceneCam(c, out->R, out->T, out->ofx, out->ofy, out->H);
}

void EObjXform::project(int vx, int vy, int vz, ProjVtx* out) const {
  const float V0 = (float)(int16_t)vx, V1 = (float)(int16_t)vy, V2 = (float)(int16_t)vz;
  out->mx = (int16_t)vx; out->my = (int16_t)vy; out->mz = (int16_t)vz;   // model coords (fps60 reproj input)
  // view = R·V + T  (R in 1.3.12 scale, so divide the rotate product by 4096; T is raw view units).
  double view[3], vz_raw = 0;
  for (int i = 0; i < 3; i++) {
    double t = (double)T[i] * 4096.0 + (double)R[i][0] * V0 + (double)R[i][1] * V1 + (double)R[i][2] * V2;
    if (i == 2) vz_raw = t;
    view[i] = t / 4096.0;
  }
  // IR saturation to ±32767 (kept so per-object geometry lines up with the rest of the projection pipeline).
  float ir1 = (float)(view[0] < -32768 ? -32768 : view[0] > 32767 ? 32767 : view[0]);
  float ir2 = (float)(view[1] < -32768 ? -32768 : view[1] > 32767 ? 32767 : view[1]);
  float ir3 = (float)(view[2] < -32768 ? -32768 : view[2] > 32767 ? 32767 : view[2]);
  out->ir1 = (int)ir1; out->ir2 = (int)ir2; out->ir3 = (int)ir3;
  out->vx = ir1; out->vy = ir2; out->vz = ir3;
  float szf = (float)(vz_raw / 4096.0);
  int32_t szi = (int32_t)szf; out->sz = szi < 0 ? 0 : szi > 65535 ? 65535 : szi;
  // perspective: pz = max(H/2, view-Z); screen = OFX/OFY + IR * (H / pz).
  float pz = H * 0.5f; if (szf > pz) pz = szf;
  float ph = (pz > 0.0f) ? H / pz : 0.0f;
  out->px = ofx + ir1 * ph;
  out->py = ofy + ir2 * ph;
  if (out->px < -1024.f) out->px = -1024.f; if (out->px > 1023.f) out->px = 1023.f;
  if (out->py < -1024.f) out->py = -1024.f; if (out->py > 1023.f) out->py = 1023.f;
  int32_t sxi = (int32_t)(out->px < 0 ? out->px - 0.5f : out->px + 0.5f);
  int32_t syi = (int32_t)(out->py < 0 ? out->py - 0.5f : out->py + 0.5f);
  out->sx = sxi < -1024 ? -1024 : sxi > 1023 ? 1023 : sxi;
  out->sy = syi < -1024 ? -1024 : syi > 1023 ? 1023 : syi;
  out->pz = pz;
}

// The active object xform: set once per render command by the per-object flush; the GT3/GT4 submitters
// project every vertex through it. There is NO GTE fallback — a submitter that runs in the per-object path
// always has an active xform.
void Render::projSetActive(const EObjXform* w) { mActiveXform = *w; mActiveXformSet = true; }
void Render::projClearActive()                 { mActiveXformSet = false; }
void Render::projVertexActive(int vx, int vy, int vz, ProjVtx* out) { mActiveXform.project(vx, vy, vz, out); }

static inline int32_t round_i16(float f) {
  int32_t v = (int32_t)(f < 0 ? f - 0.5f : f + 0.5f);
  return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}
static inline int32_t round_i32(float f) { return (int32_t)(f < 0 ? f - 0.5f : f + 0.5f); }
void Render::projActiveCr(uint32_t cr[11]) {
  // pack R (1.3.12 scale) into CR0-4 halfword layout, T into CR5-7, projection consts into cr[8..10].
  const EObjXform& a = mActiveXform;
  uint16_t R[3][3];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = (uint16_t)round_i16(a.R[i][j]);
  cr[0] = (uint32_t)R[0][0] | ((uint32_t)R[0][1] << 16);
  cr[1] = (uint32_t)R[0][2] | ((uint32_t)R[1][0] << 16);
  cr[2] = (uint32_t)R[1][1] | ((uint32_t)R[1][2] << 16);
  cr[3] = (uint32_t)R[2][0] | ((uint32_t)R[2][1] << 16);
  cr[4] = (uint32_t)R[2][2];
  cr[5] = (uint32_t)round_i32(a.T[0]);
  cr[6] = (uint32_t)round_i32(a.T[1]);
  cr[7] = (uint32_t)round_i32(a.T[2]);
  cr[8] = (uint32_t)(int32_t)(a.ofx * 65536.0f);
  cr[9] = (uint32_t)(int32_t)(a.ofy * 65536.0f);
  cr[10] = (uint32_t)(uint16_t)a.H;
}
