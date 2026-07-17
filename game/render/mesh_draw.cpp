// game/render/mesh_draw.cpp — NATIVE 3D-mesh drawer.
//
// Parses the GEOMBLK model format (the SAME layout tools/export_model.py parses and native_terrain.cpp
// renders), transforms each prim's model-space verts by the object's FLOAT model->view matrix, projects
// with the camera, and draws each prim as a textured quad/tri with REAL per-pixel depth via
// RenderQueue::drawWorldQuad. This is the decoupled native render: no GTE compose, no gte_op, no GP0 packet, no
// OT — the engine owns the transform, projection, and depth.
//
// GEOMBLK (from export_model.py docstring / submit.cpp GT3/GT4 RE):
//   header @geomblk+0: u32 counts -> low16 = #GT3 (textured tris), high16 = #GT4 (textured quads)
//   records @geomblk+16: nGT3 GT3 records (36B) then nGT4 GT4 records (44B), model-space s16 verts.
//   GT3 (36B): +0 rgb0|op +4 rgb1 +8 uv0|clut +12 uv1|tpage +16 XY0 +20 Z0|Z1
//              +24 XY1 +28 XY2 +32 Z2(lo)|uv2(hi)
//   GT4 (44B): +0 rgb0 +4 rgb2 +8 uv0|clut +12 uv1|tpage +16 uv2(lo)|uv3(hi)
//              +20 XY0 +24 Z0|Z1 +28 XY1 +32 XY2 +36 Z2|Z3 +40 XY3
//   XY = packed s16 x(low)|s16 y(high); Z = s16. tpage/clut = PSX texpage/CLUT ids.
//
// Per-vertex RGB: this first cut uses a flat white (255) modulate so the decoded VRAM TEXTURE shows
// through unmodified (export_model.py confirmed the textures decode correctly). The geomblk also carries
// per-prim RGB (rgb0/rgb1/rgb2) but the per-vertex mapping differs GT3/GT4; a faithful vertex-colour pass
// is a follow-up. Texture resolution is by the prim's PSX tpage/clut, handed to RenderQueue::drawWorldQuad which
// samples the VRAM image natively (no GP0). A native RGBA texture CACHE is the next asset-subsystem step;
// sampling VRAM by tpage/clut here is the documented first pass.
#include "core.h"
#include "cfg.h"
#include "scene_data.h"
#include "render.h"          // class Render — reach `c->rsub.projParams` for depth-normalize
#include "proj_params.h"     // class ProjParams — setProjH + proj_pz_to_ord (kept as free fn, per-Core state)
#include "render_native.h"   // class NativeScenePass — drawObject is a method here
#include <stdint.h>
#include <math.h>

// engine float world-quad draw (real depth) — RenderQueue::drawWorldQuad (game.h brings render_queue.h).
#include "game.h"

static inline int16_t s16(uint32_t v) { return (int16_t)(v & 0xFFFF); }

// Project one model-space vert through R/T (model->view) + camera projection -> screen XY + ord depth.
struct PVtx { float px, py, depth; };
static inline PVtx project(const SceneObject* o, const SceneCamera* cam, float H, float nearp,
                           int mx, int my, int mz) {
  float vx = (float)mx, vy = (float)my, vz = (float)mz;
  float w0 = o->R[0][0]*vx + o->R[0][1]*vy + o->R[0][2]*vz + o->T[0];
  float w1 = o->R[1][0]*vx + o->R[1][1]*vy + o->R[1][2]*vz + o->T[1];
  float w2 = o->R[2][0]*vx + o->R[2][1]*vy + o->R[2][2]*vz + o->T[2];
  float pz = w2; if (pz < nearp) pz = nearp;
  float ph = H / pz;
  float sx = cam->ofx + w0 * ph; if (sx < -1024.f) sx = -1024.f; if (sx > 1023.f) sx = 1023.f;
  float sy = cam->ofy + w1 * ph; if (sy < -1024.f) sy = -1024.f; if (sy > 1023.f) sy = 1023.f;
  PVtx p; p.px = sx; p.py = sy; p.depth = proj_pz_to_ord(pz);
  return p;
}

int NativeScenePass::drawObject(const SceneObject* o, const SceneCamera* cam) {
  Core* c = mCore;
  uint32_t counts = c->mem_r32(o->geomblk);
  int n3 = counts & 0xFFFF, n4 = (counts >> 16) & 0xFFFF;
  if (n3 > 4096 || n4 > 4096) return 0;                  // sanity: not a geomblk

  float H = cam->H; if (H < 1.0f) H = 1.0f;
  c->rsub.projParams.setProjH((uint16_t)H);
  float nearp = H * 0.5f; if (nearp < 1.0f) nearp = 1.0f;

  uint32_t rec = o->geomblk + 16;
  int drawn = 0;
  const uint8_t W = 255;
  uint8_t r[4] = {W,W,W,W}, g[4] = {W,W,W,W}, b[4] = {W,W,W,W};

  // --- GT3 textured tris (36B) ---
  for (int t = 0; t < n3; t++, rec += 36) {
    uint32_t xy0 = c->mem_r32(rec+16), xy1 = c->mem_r32(rec+24), xy2 = c->mem_r32(rec+28);
    uint32_t vz01 = c->mem_r32(rec+20); uint16_t z2 = c->mem_r16(rec+32);
    int X0=s16(xy0), Y0=s16(xy0>>16), Z0=s16(vz01);
    int X1=s16(xy1), Y1=s16(xy1>>16), Z1=s16(vz01>>16);
    int X2=s16(xy2), Y2=s16(xy2>>16), Z2=(int16_t)z2;
    uint32_t uv0 = c->mem_r32(rec+8), uv1 = c->mem_r32(rec+12); uint16_t uv2 = c->mem_r16(rec+34);
    uint16_t clut = (uint16_t)(uv0 >> 16), tp = (uint16_t)(uv1 >> 16);
    PVtx p0 = project(o, cam, H, nearp, X0,Y0,Z0);
    PVtx p1 = project(o, cam, H, nearp, X1,Y1,Z1);
    PVtx p2 = project(o, cam, H, nearp, X2,Y2,Z2);
    // degenerate 4th vert (= v2): tritri split (0,1,2)/(1,2,3) makes the 2nd tri zero-area.
    float px[4] = { p0.px, p1.px, p2.px, p2.px };
    float py[4] = { p0.py, p1.py, p2.py, p2.py };
    float dp[4] = { p0.depth, p1.depth, p2.depth, p2.depth };
    int   u[4]  = { (int)(uv0 & 0xFF), (int)(uv1 & 0xFF), (int)(uv2 & 0xFF), (int)(uv2 & 0xFF) };
    int   v[4]  = { (int)((uv0>>8) & 0xFF), (int)((uv1>>8) & 0xFF), (int)((uv2>>8) & 0xFF), (int)((uv2>>8) & 0xFF) };
    c->game->rq.drawWorldQuad(c, px, py, dp, u, v, r, g, b, tp, clut, 0, nullptr);
    drawn++;
  }

  // --- GT4 textured quads (44B) ---
  for (int q = 0; q < n4; q++, rec += 44) {
    uint32_t xy0 = c->mem_r32(rec+20), xy1 = c->mem_r32(rec+28), xy2 = c->mem_r32(rec+32), xy3 = c->mem_r32(rec+40);
    uint32_t vz01 = c->mem_r32(rec+24), vz23 = c->mem_r32(rec+36);
    int X0=s16(xy0), Y0=s16(xy0>>16), Z0=s16(vz01);
    int X1=s16(xy1), Y1=s16(xy1>>16), Z1=s16(vz01>>16);
    int X2=s16(xy2), Y2=s16(xy2>>16), Z2=s16(vz23);
    int X3=s16(xy3), Y3=s16(xy3>>16), Z3=s16(vz23>>16);
    uint32_t uv0 = c->mem_r32(rec+8), uv1 = c->mem_r32(rec+12), uv23 = c->mem_r32(rec+16);
    uint16_t clut = (uint16_t)(uv0 >> 16), tp = (uint16_t)(uv1 >> 16);
    PVtx p0 = project(o, cam, H, nearp, X0,Y0,Z0);
    PVtx p1 = project(o, cam, H, nearp, X1,Y1,Z1);
    PVtx p2 = project(o, cam, H, nearp, X2,Y2,Z2);
    PVtx p3 = project(o, cam, H, nearp, X3,Y3,Z3);
    float px[4] = { p0.px, p1.px, p2.px, p3.px };
    float py[4] = { p0.py, p1.py, p2.py, p3.py };
    float dp[4] = { p0.depth, p1.depth, p2.depth, p3.depth };
    int   u[4]  = { (int)(uv0 & 0xFF), (int)(uv1 & 0xFF), (int)(uv23 & 0xFF), (int)((uv23>>16) & 0xFF) };
    int   v[4]  = { (int)((uv0>>8) & 0xFF), (int)((uv1>>8) & 0xFF), (int)((uv23>>8) & 0xFF), (int)((uv23>>24) & 0xFF) };
    c->game->rq.drawWorldQuad(c, px, py, dp, u, v, r, g, b, tp, clut, 0, nullptr);
    drawn++;
  }
  return drawn;
}
