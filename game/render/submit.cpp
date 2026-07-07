// Native ownership of the engine's geometry SUBMIT path (Tomba2Engine).
//
// These are the resident routines that take an object's pre-built primitive-record list, GTE-project
// each record's model vertices, backface/frustum-cull, compute an ordering-table (OT) bucket, write the
// screen-space GPU packet, and link it into the OT. The recompiled MIPS bodies threw away the float
// view-space depth (only integer SXY survives into the packet), which is the ONLY reason the value-keyed
// "attach" measurement-hack existed (recovering depth by correlating projected SXY against memory
// stores). By owning the submit code natively we compute the projection and KEEP the real per-vertex
// view-Z, carrying it straight to the renderer's depth path — no correlation, no bridge.
//
// Faithful-first: the native routine reproduces the recomp body BYTE-FOR-BYTE (identical packets, OT
// links, packet-pool advance, cull decisions, return value), verified 0-diff vs the recomp body on real
// field gameplay. The GTE math itself stays a
// platform primitive (gte_op → the Beetle GTE), exactly as the recomp body called it, so projection
// results are bit-identical; we own the control flow, record decode, packet assembly and OT insertion.
//
// RE (recomp bodies gen_func_8007FDB0 / gen_func_8008007C, decoded into clean form — docs/engine_re.md):
//   args: a0 = primitive-record array, a1 = OT base, a2 = record count;  returns a0 advanced past the array.
//   global packet-pool write pointer at 0x800BF544 (advanced past each committed packet).
//
// NOTE (2026-07 restructure): game/render/render_native.cpp (+ render/scene_build.cpp +
// render/mesh_draw.cpp) is the CLAUDE.md-mandated eventual replacement for this file.
#include "core.h"
#include "game.h"   // Fps60::current_object (was g_current_object)
#include "cfg.h"
#include "mods.h"   // g_mods — live PC-native lighting params (engine-native shading, not a deferred pass)
#include "lighting.h" // PER-AREA light registry (sun / lava+torch); selected per frame in Render::shadeSelect
#include "render_queue.h" // RQ_BACKGROUND + RenderQueue::push2dQuad — native backdrop tilemap path
#include "render_internal.h" // shared render internals (PktSpanSession, obj_world_ord)
#include "gte_math.h"     // Math:: — GTE-transform cluster (matMul/applyMatlv/rotX/Y/Z/rotmat, static)
#include "mtx.h"              // class Mtx — libgte helpers (identity, diagonal, ...)
#include "trig.h"             // class Trig — libgte rsin/rcos
#include "render.h"           // class Render — Render::fieldEntityRender lives here
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// g_dbg_cur_geomblk retired — per-Core Render::mDbgCurGeomblk (sil_bbox_log_node diag)
#include <math.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

// ---------------------------------------------------------------------------------------------------
// NESTING-SAFE packet-pool span tracking (issue #4 — ropes/flames drew over terrain).
//
// Per-object depth tagging captures the address span an object's renderer writes into the packet pool
// (g_pkt_track/g_pkt_lo/g_pkt_hi in mem.cpp), then stamps it with the object's world depth. The three
// globals are ONE shared session, so the sessions did NOT nest. The rope/flame objects are rendered by
// an aux walk (BCF4) whose per-type renderer (overlay fn 0x801341E8 / 0x80136748) INTERNALLY dispatches
// the universal per-object render command 0x8003F698 (ov_render_cmd) for the SAME node. ov_render_cmd
// opened its own session and, on exit, reset g_pkt_track=0 — so the rope's own quads, emitted by the
// renderer AFTER that inner dispatch (via the per-quad submitter 0x8003B320 into 0x800C0xxx), were
// written with tracking OFF. They got NO span, missed obj_depth_lookup at the OT walk, and fell to the
// flat 2D OVERLAY band — drawing over the terrain/foliage. (Diagnosed live at tp 11647 -1597 2352:
// idx-1 PRE track=1 -> inner ov_render_cmd -> POST track=0; 63/73 2D prims MISS.)
//
// Fix: a session SAVES the outer session's state on open and, on close, RESTORES it while MERGING its own
// [lo,hi) into the outer's. The inner ov_render_cmd still publishes its own span, but tracking resumes
// for the OUTER (rope-walk) session, so the outer's final gpu_obj_depth_add covers ALL of the object's
// packets — the flush quads AND the rope segment quads — with the object's world depth. Nesting now works
// for every span-tracking site (this is the general fix, not a rope special-case).
// PktSpanSession + the nesting-safe packet-span rationale now live in render_internal.h (shared with
// render_walk.cpp).

#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to RGB words (matches the GPU)


// Right-edge frustum-cull threshold. The submit drops a prim only if ALL its verts are off the right of
// the screen. In 4:3 that's SX>=320 (faithful). Genuine engine-wide (gpu_gpu_wide_engine) extends the
// screen to the wide width (428@16:9), so geometry projected into the [320,wide) right band is ON-screen
// and MUST NOT be dropped — widen the threshold to the wide width. THIS is why the right-side terrain was
// missing in wide: the engine's own submit culled it to 4:3. (Vertical 240 cull unchanged.) later-119.
int gpu_gpu_wide_engine(void), gpu_gpu_wide_engine_w(void);
static int submit_xmax(void) { return gpu_gpu_wide_engine() ? gpu_gpu_wide_engine_w() : 320; }

// PSXPORT_DEBUG=geomblk — geometry-record CAPTURE probe. Dumps the RAW primitive records of every geomblk
// submitted through the three natively-owned submitters (GT3 0x8007FDB0, GT4 0x8008007C, GT4bp 0x80027768).
// The raw record bytes are the canonical INPUT a native render-half must reproduce byte-for-byte (the 0-diff
// gate). stride = per-prim record size (36 GT3 / 44 GT4 / 36 GT4bp).
//
// IMPORTANT — object attribution is NOT available from here (journal later-130). Geometry SUBMISSION is a
// DEFERRED FLUSH phase, decoupled from the per-object entity walk: at the field, all owned submits run with
// g_current_object == 0 (outside any handler's node_call context) and AFTER every per-object cull. So neither
// the walk-tap (g_current_object → 0 at flush) nor the cull-tap (g_render_object → stuck at the last-culled
// object) names the geomblk's source object. We log both as weak context only; true attribution needs tapping
// the geomblk ENQUEUE site (where a handler registers its render command, while g_current_object = its node).
// At the field the owned path carries the world/map renderer's geometry; the 78 margin objects enqueue via
// UN-owned submitter variants and are invisible here. Off by default; pure logging, no state change.
int gpu_frame_no(Core*);
void gpu_obj_depth_add(Core*, uint32_t lo, uint32_t hi, float ord);
float proj_obj_center_ord(void);
// PC-NATIVE object depth from real 3D placement. proj_camview_world_ord projects a WORLD point through the
// STABLE scene camera (published once per frame at terrain draw, camview_publish); camview_valid says it's
// known. This is the engine owning object depth from the object's spawned world position — NOT the volatile
// live-GTE origin projection (proj_obj_center_ord reads whatever camera×object transform was composed LAST,
// so render ORDER leaks into the depth and billboards get a wrong/too-far view-Z, losing the depth test to
// terrain and vanishing). See obj_world_ord below.
// class ProjParams (game/render/proj_params.h) — per-Core; the header brings in the free-function bridges
// (proj_pz_to_ord, proj_set_H, proj_camview_world_ord, camview_valid) that used to be declared inline here.
#include "proj_params.h"
// g_fps60_on retired — read g_mods.fps60 (mods.h)
// The entity node the native render walk is currently rendering (set around each per-object dispatch,
// below). The PER-INSTANCE identity for every prim an object emits — including a 2D billboard whose quad
// rasterizes later at the OT walk. Used both for the objid overlay (RenderQueue::emitOrQueue) and as the
// billboard span identity (so collectables/flames are identified individually, not merged under a shared id).
// g_dbg_render_node retired — per-Core Render::mDbgRenderNode (see render.h)
// cur_render_node + obj_world_ord (PC-native per-object depth) now live in render_internal.h (shared).
#define PKT_POOL_PTR  0x800BF544u

// PC-native per-vertex depth (Phase 2): because we OWN the projection, we know each vertex's real
// view-space Z (the SZ the GTE just produced) — record it keyed by the packet vertex word's address so
// the renderer's D32 depth buffer does true per-pixel occlusion (PSXPORT_NATIVE_DEPTH / the SBS A/B
// view) instead of OT-submission order. No correlation, no value-matching: the engine that emits the
// vertex writes the depth for the exact address it stored the SXY to. Off (faithful) by default.
// PC-NATIVE render path. ProjVtx + the per-object world-coord projection live in game/render/projection.*.
// proj_native_xform (gte_beetle) is the GTE-composed-transform projection still used by the resident
// byte-packed GT4 emitter (submit_poly_gt4_bp), whose upstream compose is the still-PSX field code.
#include "projection.h"
void  proj_native_xform(int vx, int vy, int vz, ProjVtx* out);
int  gpu_gpu_shadows_active(void);
// Fill `vv` with the prim's 4 view-space verts (x=ir1=vx, y=ir2=vy, z=pz) — the shadow VBO input — and
// return a pointer to it (NULL when this prim doesn't cast: semi, or shadows off). The shadow geometry is
// then carried ON the queued RqItem (RenderQueue::drawWorldQuad's sv arg) so it is rebuilt per present pass from
// the queue, NOT pushed here into a side stream — that is what removes the keep_shadow strobe hack.
static inline const float (*shadow_verts(const ProjVtx* p, int nv, int semi, float vv[4][3]))[3] {
  if (semi || !gpu_gpu_shadows_active()) return nullptr;          // only opaque world casts shadows
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    vv[k][0] = p[s].vx; vv[k][1] = p[s].vy; vv[k][2] = p[s].pz; }   // view space (x=ir1, y=ir2, z=pz)
  return vv;
}
// fps60: after a GTE-composed world quad is pushed to the render queue, capture its model verts + the
// composed transform (CR0-7) + the current actor key so the 60fps tier can reproject it at the A/B
// midpoint (engine/fps60.cpp). No-op unless g_mods.fps60. mv[k] = per-vertex model coords (4th = v2 for tris).
// (fps60 gate is g_mods.fps60; the capture hooks are Fps60 methods on c->game->fps60.)

// fps60: 3D-POSITIONED 2D QUAD (billboard) capture. The collectable/flame/decal billboards are guest GP0
// quads/sprites the per-object renderers emit; they reach the render queue LATER, at the deferred OT walk
// (gpu_native.cpp, off the engine-submit path), where they inherit the object's WORLD-POSITION depth via
// obj_depth_lookup — so they carry fps_world=0 and the 60fps tier snapped them to camera-B (they juddered
// while Tomba moved smoothly). We tag them at QUEUE TIME from the OT walk now: we record an fps60 BILLBOARD
// entry (Fps60::recordBillboardSpan) at the SAME instant we publish the object's depth span — keyed
// by that SPAN [lo,hi) (the OT walk matches each billboard item's source node against it, identical to
// obj_depth_lookup) + the object's stable cross-frame identity (`ident`, the node/cmd ptr) + the live
// composed camera×object transform. The OT walk (gpu_native.cpp) then stamps each billboard item directly
// as an anchor-reproject billboard, and build_lerp reprojects its WORLD ANCHOR at the midpoint camera — the
// same anchor-translate the mesh path uses, keyed on identity (not depth ord). Host-only; no guest write.
// fps60_bb_node now lives in render_internal.h (shared with render_walk.cpp).
static inline void fps60_stamp(Core* c, const ProjVtx* p, int nv) {
  if (!g_mods.fps60) return;
  int16_t mv[4][3];
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    mv[k][0] = (int16_t)p[s].mx; mv[k][1] = (int16_t)p[s].my; mv[k][2] = (int16_t)p[s].mz; }
  // World-coord native path: capture the composed transform from the active float xform; GTE path (the
  // resident byte-packed emitter) falls back to reading the live control registers.
  if (c->mRender->projActive()) { uint32_t cr[11]; c->mRender->projActiveCr(cr); c->game->fps60.stampWorldCr(c, mv, nv, c->game->fps60.fps_cur_key, cr); }
  else                  c->game->fps60.stampWorld(c, mv, nv, c->game->fps60.fps_cur_key);
}

// ENGINE-NATIVE directional lighting (user directive 2026-06-21: lighting must be engine-native, NOT a
// screen-space deferred pass). Compute a real per-FACE normal from the prim's own view-space geometry
// (cross of two edges of ProjVtx.vx/vy/vz = the GTE-rotated vertex = view-space position) and modulate the
// vertex colours by ambient + diffuse*max(0,N·L). This shades ONLY the opaque world geometry it is called
// on — it never touches semi-transparent surfaces (water etc.), so translucency is unaffected by
// construction (that was the deferred pass's bug: it re-shaded/clobbered pixels behind translucent water).
// Light dir is the to-light vector in view space (g_mods.light_dir), same convention as the retired pass.
// PER-AREA lighting (engine/lighting.cpp): the directional light is now COLOURED and AREA-SELECTED (open
// areas get a warm SUN, mines get a dim cave ambient), plus optional POINT lights (lava up-glow / torches)
// attenuated by the face's view-space position. Config picked once per frame in Render::shadeSelect() (the
// renderer caches it so this hot per-face routine doesn't re-read guest RAM). lit is now per-CHANNEL: each
// vertex colour is modulated by (ambient_col*ambient + dir_col*diffuse*N·L + Σ point_col*att*N·L).
void Render::shadeSelect() {                 // called once per world frame before the submitters run
  mShadeCfg = lighting.select(lighting.areaKeyFrom(mCore));
}
static inline void engine_shade_face(Core* c, const ProjVtx* p, int nv, uint8_t r[4], uint8_t g[4], uint8_t b[4]) {
  if (!g_mods.light) return;
  Render* rr = c->mRender;
  const LightConfig* cfg = rr->mShadeCfg ? rr->mShadeCfg : rr->lighting.defaultConfig();
  float e1x = p[1].vx - p[0].vx, e1y = p[1].vy - p[0].vy, e1z = p[1].vz - p[0].vz;
  float e2x = p[2].vx - p[0].vx, e2y = p[2].vy - p[0].vy, e2z = p[2].vz - p[0].vz;
  float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
  float ln = sqrtf(nx*nx + ny*ny + nz*nz); if (ln < 1e-6f) return;
  nx /= ln; ny /= ln; nz /= ln;
  if (nz > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }            // face the camera (view -Z)
  // directional key light (coloured): ambient_col*ambient + dir_col*diffuse*max(0,N·L).
  float lx = cfg->dir[0], ly = cfg->dir[1], lz = cfg->dir[2];
  float ll = sqrtf(lx*lx + ly*ly + lz*lz); if (ll > 1e-6f) { lx /= ll; ly /= ll; lz /= ll; }
  float ndl = nx*lx + ny*ly + nz*lz; if (ndl < 0.0f) ndl = 0.0f;
  float litR = cfg->ambient_color[0]*cfg->ambient + cfg->dir_color[0]*cfg->dir_intensity*ndl;
  float litG = cfg->ambient_color[1]*cfg->ambient + cfg->dir_color[1]*cfg->dir_intensity*ndl;
  float litB = cfg->ambient_color[2]*cfg->ambient + cfg->dir_color[2]*cfg->dir_intensity*ndl;
  // point lights (lava up-glow / torches): face-centre view-space pos, soft falloff to radius, N·L diffuse.
  if (cfg->num_points > 0) {
    float cxp = (p[0].vx + p[1].vx + p[2].vx) * (1.0f/3.0f);
    float cyp = (p[0].vy + p[1].vy + p[2].vy) * (1.0f/3.0f);
    float czp = (p[0].vz + p[1].vz + p[2].vz) * (1.0f/3.0f);
    for (int i = 0; i < cfg->num_points; i++) {
      const PointLight* pl = &cfg->points[i];
      float dx = pl->pos[0]-cxp, dy = pl->pos[1]-cyp, dz = pl->pos[2]-czp;
      float d = sqrtf(dx*dx+dy*dy+dz*dz);
      float rad = pl->radius > 1.0f ? pl->radius : 1.0f;
      float att = 1.0f - d/rad; if (att < 0.0f) att = 0.0f; att *= att;   // soft quadratic falloff
      float pdl = (d > 1e-3f) ? (nx*dx + ny*dy + nz*dz)/d : 0.0f; if (pdl < 0.0f) pdl = 0.0f;
      float w = pl->intensity * att * pdl;
      litR += pl->color[0]*w; litG += pl->color[1]*w; litB += pl->color[2]*w;
    }
  }
  for (int k = 0; k < nv; k++) {
    int rr = (int)(r[k] * litR + 0.5f), gg = (int)(g[k] * litG + 0.5f), bb = (int)(b[k] * litB + 0.5f);
    r[k] = (uint8_t)(rr > 255 ? 255 : rr); g[k] = (uint8_t)(gg > 255 ? 255 : gg); b[k] = (uint8_t)(bb > 255 ? 255 : bb);
  }
}
// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// PC-NATIVE POLY_GT3 submit — project the 3 model verts through the engine's composed transform in FLOAT
// (proj_native_xform, no gte_op) and tee a degenerate quad (v2 repeated) to the VK rasterizer with real
// per-pixel depth. No GP0 packet, no OT, no guest write.
void Render::submitPolyGt3Native(Core* c) {
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) fprintf(stderr,"[subc] gt3_native %ld\n", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    uint32_t vz01 = c->mem_r32(rec + 20);
    uint32_t xy0 = c->mem_r32(rec + 16), xy1 = c->mem_r32(rec + 24), xy2 = c->mem_r32(rec + 28);
    ProjVtx p[3];
    c->mRender->projVertexActive( (int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,         &p[0]);
    c->mRender->projVertexActive( (int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16), &p[1]);
    c->mRender->projVertexActive( (int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)c->mem_r32(rec + 32), &p[2]);
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface
    int xmax = submit_xmax();
    if (p[0].sx >= xmax && p[1].sx >= xmax && p[2].sx >= xmax) continue;
    if (p[0].sy >= 240 && p[1].sy >= 240 && p[2].sy >= 240) continue;
    uint32_t code = c->mem_r32(rec + 0);                      // rgb0|op ; rgb1 @rec+4, rgb2 = rgb1<<4
    uint32_t rgb[3] = { code & COL_MASK, c->mem_r32(rec + 4) & COL_MASK, (c->mem_r32(rec + 4) << 4) & COL_MASK };
    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12);
    uint16_t clut = (uint16_t)(uv0 >> 16), tp = (uint16_t)(uv1 >> 16);
    int u[4], v[4]; uint8_t r[4], g[4], b[4]; float px[4], py[4], depth[4];
    u[0] = uv0 & 0xFF;  v[0] = (uv0 >> 8) & 0xFF;
    u[1] = uv1 & 0xFF;  v[1] = (uv1 >> 8) & 0xFF;
    u[2] = c->mem_r16(rec + 34) & 0xFF; v[2] = (c->mem_r16(rec + 34) >> 8) & 0xFF;   // uv2 (high half of rec+32)
    for (int k = 0; k < 3; k++) {
      px[k] = p[k].px; py[k] = p[k].py; depth[k] = proj_pz_to_ord(p[k].pz);
      r[k] = rgb[k] & 0xFF; g[k] = (rgb[k] >> 8) & 0xFF; b[k] = (rgb[k] >> 16) & 0xFF;
    }
    px[3] = px[2]; py[3] = py[2]; depth[3] = depth[2];        // 4th vert = v2 (degenerate -> a triangle)
    u[3] = u[2]; v[3] = v[2]; r[3] = r[2]; g[3] = g[2]; b[3] = b[2];
    int semi = (code & 0x02000000) ? 1 : 0;
    if (!semi) engine_shade_face(c, p, 3, r, g, b);             // engine-native lighting (opaque only)
    { char tag[32]; snprintf(tag, sizeof tag, "gt3_native@%08X", c->mRender->diag.currentGeomblk()); sil_bbox_log_verts(tag, px, py, depth, 3, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 3, semi, vv);   // dynamic shadow verts (carried on the item)
      c->game->rq.drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
    fps60_stamp(c, p, 3);                                    // fps60: capture for midpoint reprojection
  }
  c->r[2] = rec;
}

// gen_func_8008007C — POLY_GT4 (gouraud-textured quad) submit, PC-NATIVE.
// Record = 44 bytes: {+0 rgb0(rgb1=<<4), +4 rgb2(rgb3=<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 uv2(lo)|uv3(hi), +20 VXY0, +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi), +40 VXY3}.
// Project the 4 model verts through the engine's composed transform in FLOAT (proj_native_xform, no
// gte_op) and tee the quad straight to the VK rasterizer (RenderQueue::drawWorldQuad) with real per-pixel depth.
// NO GP0 packet, NO OT, NO guest write — the renderer a PC game has. Cull rules (backface/frustum) are
// reproduced on the native projection so we drop the same prims the engine would. Returns the advanced
// record pointer (the engine reads it back).
void Render::submitPolyGt4Native(Core* c) {
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) fprintf(stderr,"[subc] gt4_native %ld\n", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    // model verts: V0=rec+20(XY)|rec+24.lo(Z), V1=rec+28|rec+24.hi, V2=rec+32|rec+36.lo, V3=rec+40|rec+36.hi
    uint32_t vz01 = c->mem_r32(rec + 24), vz23 = c->mem_r32(rec + 36);
    uint32_t xy0 = c->mem_r32(rec + 20), xy1 = c->mem_r32(rec + 28),
             xy2 = c->mem_r32(rec + 32), xy3 = c->mem_r32(rec + 40);
    ProjVtx p[4];
    c->mRender->projVertexActive( (int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,          &p[0]);
    c->mRender->projVertexActive( (int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16),  &p[1]);
    c->mRender->projVertexActive( (int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)vz23,          &p[2]);
    c->mRender->projVertexActive( (int16_t)xy3, (int16_t)(xy3 >> 16), (int16_t)(vz23 >> 16),  &p[3]);
    // backface cull on the FRONT triangle's signed screen area (NCLIP: (SX1-SX0)*(SY2-SY0)-(SX2-SX0)*(SY1-SY0)).
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface (matches MAC0<=0 drop)
    // frustum cull (right/bottom edges only, as the original) over all 4 verts.
    int xmax = submit_xmax();
    if (p[0].sx >= xmax && p[1].sx >= xmax && p[2].sx >= xmax && p[3].sx >= xmax) continue;
    if (p[0].sy >= 240 && p[1].sy >= 240 && p[2].sy >= 240 && p[3].sy >= 240) continue;
    // decode RGB (rgb0 @rec+0, rgb1=rgb0<<4; rgb2 @rec+4, rgb3=rgb2<<4) and UV/CLUT/texpage.
    uint32_t code0 = c->mem_r32(rec + 0), code2 = c->mem_r32(rec + 4);
    uint32_t rgb[4] = { code0 & COL_MASK, (code0 << 4) & COL_MASK, code2 & COL_MASK, (code2 << 4) & COL_MASK };
    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12), uv23 = c->mem_r32(rec + 16);
    uint16_t clut = (uint16_t)(uv0 >> 16);
    uint16_t tp   = (uint16_t)(uv1 >> 16);
    int u[4], v[4]; uint8_t r[4], g[4], b[4]; float px[4], py[4], depth[4];
    u[0] = uv0 & 0xFF;        v[0] = (uv0 >> 8) & 0xFF;
    u[1] = uv1 & 0xFF;        v[1] = (uv1 >> 8) & 0xFF;
    u[2] = uv23 & 0xFF;       v[2] = (uv23 >> 8) & 0xFF;
    u[3] = (uv23 >> 16) & 0xFF; v[3] = (uv23 >> 24) & 0xFF;
    for (int k = 0; k < 4; k++) {
      px[k] = p[k].px; py[k] = p[k].py; depth[k] = proj_pz_to_ord(p[k].pz);
      r[k] = rgb[k] & 0xFF; g[k] = (rgb[k] >> 8) & 0xFF; b[k] = (rgb[k] >> 16) & 0xFF;
    }
    int semi = (code0 & 0x02000000) ? 1 : 0;                  // GP0 op byte (code0>>24) bit1 = semi-transparency
    if (!semi) engine_shade_face(c, p, 4, r, g, b);             // engine-native lighting (opaque only)
    { char tag[32]; snprintf(tag, sizeof tag, "gt4_native@%08X", c->mRender->diag.currentGeomblk()); sil_bbox_log_verts(tag, px, py, depth, 4, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 4, semi, vv);   // dynamic shadow verts (carried on the item)
      c->game->rq.drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
    fps60_stamp(c, p, 4);                                    // fps60: capture for midpoint reprojection
  }
  c->r[2] = rec;                                              // return: record pointer advanced past the array
}

// =====================================================================================================
// NATIVE PER-OBJECT RENDER FLUSH — gen_func_8003CDD8 (THE world/margin render submission, later-133).
//
// This is the heart of "make it a PC game": the engine's per-object render — composing the camera ×
// object-local transform and dispatching each object's persistent render-command list to the geometry
// submitter — reimplemented in native C so NO guest render code runs (no gen_func_8003CDD8, no
// gen_func_8003F698 dispatcher, no gen_func_800803DC) and NO guest packet/VRAM is touched beyond the
// 1-word OT ordering node the native submitters already own. Decoded byte-for-byte from the recomp body
// (docs/engine_re.md "Deferred render pipeline" / journal later-133):
//
//   gen_func_8003CDD8(a0=node, a1=flag): for each render command in the node's persistent list
//   (count at node+8 / node+9, cmd-ptr ARRAY at node+0xc0[i]):
//     - geomblk = cmd+0x40; skip the command if it is 0.
//     - COMPOSE the GTE transform: camera-rotation (scratch 0x1F8000F8 → CR0-4) × the object-local
//       matrix (cmd+0x18, 3 columns at +0x18/+0x1a/+0x1c, each col 3 halfwords at +0,+6,+0xc) via one
//       MVMVA (0x4A49E012, mx=0 rotation, v=3 IR vector) per column → composed rotation matrix.
//     - TRANSFORM the object translation (cmd+0x2c/0x30/0x34) by the camera (MVMVA 0x4A486012, v=0 V0)
//       then ADD the camera translation offset (scratch 0x1F80010C/110/114) → composed translation.
//     - Load the composed rotation into CR0-4 and translation into CR5-7.
//     - Dispatch geomblk to the per-mode renderer with OT base *0x800ED8C8 (+cmd[0x3f]*4 when
//       node[0xd]&0xf == 4) and the flush flag.
//
// The MVMVA matrix math stays a platform primitive (gte_op → the Beetle GTE), exactly as the recomp
// body called it, so the composed CR0-7 are bit-identical. The scratchpad temps (0x1F8000xx) are the
// SAME the recomp body uses — pure CPU scratch, not render packet/VRAM. The dispatch routes the common
// world path natively (native_dispatch → Render::gt3gt4 → the native submitPolyGt3/Gt4Native above);
// the per-scene OVERLAY submitter variants (mode-table entries other than the GT3/GT4 path) are NOT yet
// owned, so for those modes the original per-mode renderer is invoked (rec_dispatch) — the documented
// next RE target (engine_re "OPEN — full field depth coverage").
#define SCR          0x1F800000u             // PSX scratchpad base (the engine's GTE-compose temp area)
#define MODE_BYTE    0x800BF870u             // *this = render-mode select (DAT_800bf870, 0..0x15)
#define MODE_FORCE   0x1F800234u             // *this != 0 forces the generic GT3/GT4 path
#define MODE_TABLE   0x80015268u             // 22-entry jump table: mode → per-mode renderer
#define MVMVA_ROTCOL 0x4A49E012u             // MVMVA: camera-rot(CR0-4) × IR vector → composed col
#define MVMVA_TRANS  0x4A486012u             // MVMVA: camera-rot × V0 (object translation)

void rec_dispatch(Core*, uint32_t);         // interpret/run a guest fn (unowned overlay-variant modes)

// gen_func_800803DC's first body (the generic GT3/GT4 renderer): split the geomblk's packed prim counts
// (low16 tri, high16 quad), point past the 16-byte header to the record array, and run the two native
// submitters in sequence (tri-submit returns the advanced record pointer = the quad array base).
// g_dbg_cur_geomblk retired — per-Core Render::mDbgCurGeomblk
void Render::gt3gt4(uint32_t geomblk, uint32_t otbase) {   // used by render_walk.cpp
  Core* c = mCore;
  diag.setGeomblk(geomblk);
  uint32_t counts = c->mem_r32(geomblk + 0);
  c->r[4] = geomblk + 16; c->r[5] = otbase; c->r[6] = counts & 0xFFFFu;
  submitPolyGt3Native(c);
  c->r[4] = c->r[2];      c->r[5] = otbase; c->r[6] = counts >> 16;
  submitPolyGt4Native(c);
}

// FIELD ENTITY RENDER LOOP — PC-native ownership of the SOP field-overlay entity render 0x80109fe0
// (sop.cpp:203, "entity render loop"). The field loads the scene CAMERA into the GTE once, then submits
// each entity's GT3/GT4 geometry whose vertices are already in WORLD space. We own it natively: build the
// float camera-view transform from world coordinates (Render::projComposeCamera) and route every entity through
// the native GT3/GT4 submitters — projecting in float through that world-coord transform, with real depth.
// NO gte_op, NO PSX submit library (0x801099b4 / 0x80109c80). This is the path that actually draws the
// visible field (Tomba, props, terrain props); before this it ran 100% interpreted.
//
// Faithful transcription of the loop's addressing (decoded from the overlay disasm):
//   a0 = entity-list struct: [6] = u8 count, [0xc] = packed-geometry base, [0x10..] = u16 entry offsets.
//   per entry: cmd = base + idx*4; s0 = *cmd (packed counts); records follow at cmd+4.
//     GT3 submit(rec = cmd+4, ot, count = s0 & 0xff) -> returns the advanced record ptr = the GT4 base.
//     GT4 submit(rec = <ret>,  ot, count = (s0 >> 16) & 0xff).
void Render::fieldEntityRender(uint32_t es) {
  Core* c = mCore;
  uint8_t count = c->mem_r8(es + 6);
  if (count == 0) return;
  uint32_t otbase = c->mem_r32(0x800ED8C8u);              // *this = the active ordering-table base
  uint32_t base   = c->mem_r32(es + 0xC);
  EObjXform w; c->mRender->projComposeCamera(&w); c->mRender->projSetActive(&w);
  // DIAG groundproj: log the camera xform + first GT4 record's model verts and their eproj projection, so we
  // can see whether the world-space scene-table geometry projects on-screen with sane depth. (later-231b)
  if (cfg_dbg("groundproj")) { static int n=0; if (n++ < 3) {
    fprintf(stderr, "[groundproj] es=%08x count=%u base=%08x T=(%.0f,%.0f,%.0f) H=%.0f R0=(%.3f,%.3f,%.3f)\n",
      es, count, base, (double)w.T[0],(double)w.T[1],(double)w.T[2],(double)w.H,
      (double)w.R[0][0]/4096,(double)w.R[0][1]/4096,(double)w.R[0][2]/4096);
    uint32_t cmd0 = base + (uint32_t)c->mem_r16(es+0x10)*4; uint32_t s0d=c->mem_r32(cmd0);
    uint32_t rec = cmd0+4 + ((s0d&0xFF)*36);   // skip GT3s to first GT4 record (44B)
    for (int k=0;k<2;k++){ uint32_t r2=rec+k*44;
      int16_t vx=c->mem_r16s(r2+20), vy=(int16_t)(c->mem_r32(r2+20)>>16), vz=c->mem_r16s(r2+24);
      ProjVtx pv; c->mRender->projVertexActive( vx,vy,vz,&pv);
      fprintf(stderr,"   gt4[%d] model=(%d,%d,%d) -> px=%.1f py=%.1f pz=%.1f sx=%d sy=%d\n",
        k, vx,vy,vz, (double)pv.px,(double)pv.py,(double)pv.pz, pv.sx, pv.sy); } } }
  uint32_t p = es + 0x10, end = es + 0x10 + (uint32_t)count * 2;
  for (; p < end; p += 2) {
    uint32_t cmd = base + (uint32_t)c->mem_r16(p) * 4;
    uint32_t s0  = c->mem_r32(cmd);
    c->game->fps60.fps_cur_key = cmd;                                  // fps60: per-entity reproject key
    c->mRender->diag.setGeomblk(cmd);   // sil_bbox_log diag: tag this entity's cmd record (Render::gt3gt4 is NOT the caller here)
    c->r[4] = cmd + 4;  c->r[5] = otbase; c->r[6] = s0 & 0xFF;          submitPolyGt3Native(c);
    c->r[4] = c->r[2];  c->r[5] = otbase; c->r[6] = (s0 >> 16) & 0xFF;  submitPolyGt4Native(c);
    c->game->fps60.fps_cur_key = 0;
  }
  c->mRender->projClearActive();
}

// ov_ground_probe (diagnostic, `debug groundprobe`) moved to render_debug_probes.cpp (2026-07 restructure).

// NATIVE field TERRAIN renderer — gen_func_8002AB5C (later-135). The render fn (node+24) of the field's
// t32 render-list node: the bulk map/terrain geometry. Interpreted-only (reached via fn-ptr; seeded into
// the RE set). Decoded from the recomp body — it is structurally the per-object flush specialised for the
// terrain strip: set the depth-cue, build the object matrix (euler + a secondary sway), compose it with
// the camera via the same MVMVA columns as 8003CDD8, then submit the terrain prim records through the
// already-owned byte-packed submitter 0x80027768. The matrix-build leaves (80085480 euler→matrix,
// 80084520 secondary rotate) and the submit stay platform primitives (rec_dispatch / the owned override),
// exactly as the recomp body calls them; we own the orchestration, scratch writes and GTE compose.
//   - FarColor (CR21-23) = 0 (fog toward black); IR0 depth-cue factor (0x1F800090, read by 80027768) =
//     (128 - node[78]) << 5.
//   - two sway angle bytes at 0x800A2014/2016 = (node[64]*node[80])>>11 and (node[66]*node[80])>>11.
//   - terrain geomblk = 0x8009FAE8 (fixed per-frame record buffer); a1=a2=a3=0.
#define A2_PARAM     0x800A2014u             // 3-byte sway-angle param scratch (engine global)
#define IR0_STAGE    0x1F800090u             // IR0 depth-cue factor staged for the 0x80027768 submitter
// Terrain prim-record buffer (a0 to 80027768). The recomp body 0x8002AB5C loads `lui 0x800A; addiu -1304`
// = 0x8009FAE8 (confirmed: all three real callers of 0x80027768 pass -1304; 0x800A1AE8 is referenced by
// NO function as a geomblk — it was a fabricated address in the prior native port that read the WRONG
// buffer → garbage/water terrain geometry instead of the actual field strip).
#define MVMVA_TERRAIN_GEOMBLK 0x8009FAE8u
// Shared terrain scene-data prep (the faithful gameplay half): write the depth-cue regs + the two sway
// gameplay bytes, then build the object rotation matrix at scratch SCR (euler 0x80085480 + secondary
// sway 0x80084520). Used by the PC-native NativeScenePass::terrainRender (native_terrain.cpp); the verified
// sway-byte writes (later-157, A/B RAM-0-diff) have a single source of truth. Leaves the object matrix at SCR; camera matrix is at
// SCR+0xF8 (set earlier). The matrix-build leaves stay platform primitives (rec_dispatch), as the
// recomp body calls them.
void Render::prepObjectMatrix(uint32_t node) {
  Core* c = mCore;
  // depth-cue: FarColor=0, IR0 factor staged for the submitter
  gte_write_ctrl(21, 0); gte_write_ctrl(22, 0); gte_write_ctrl(23, 0);
  uint32_t ir0 = (uint32_t)((128 - c->mem_r16s(node + 78)) << 5);
  int32_t a80 = c->mem_r16s(node + 80);
  // The two sway-angle bytes (0x800A2014/2016) are written by the recomp terrain body and read back
  // by it (scaled <<2) into the secondary-rotation args; the middle byte 0x800A2015 is set elsewhere.
  // We write them to guest exactly as the recomp does (the no-guest-write rule was discarded — these
  // are part of the function's faithful behavior, and leaving them stale was the only true-gameplay
  // divergence vs the recomp body, root-caused via the A/B RAM diff). Compute, store, use.
  uint8_t sway0 = (uint8_t)((c->mem_r16s(node + 64) * a80) >> 11);
  uint8_t sway2 = (uint8_t)((c->mem_r16s(node + 66) * a80) >> 11);
  c->mem_w8(A2_PARAM + 0, sway0);
  c->mem_w8(A2_PARAM + 2, sway2);
  uint8_t sway1 = c->mem_r8(A2_PARAM + 1);                 // external (set elsewhere)
  c->mem_w32(IR0_STAGE, ir0);
  // build object rotation matrix at scratch SCR from the node's euler angles (node+84/86/88)
  c->math.rotmat(node + 84, SCR);
  // Secondary sway rotation by the host-computed angle bytes (scaled <<2). The recomp body 0x8002AB5C
  // stages these three angle words on its OWN STACK FRAME (r29 -= 56; words at r29+16/20/24), NOT in
  // scratchpad — and passes that stack pointer as 0x80084520's arg. The prior native code wrote them to
  // scratchpad 0x1F8001C0 instead, a guest write the recomp NEVER makes; that clobbered whatever live
  // engine state occupied 0x1F8001C0, corrupting gameplay (terrain collision → Tomba fell through). It
  // was invisible to the later-157 A/B gate, which diffs only the 2 MB main RAM, not the scratchpad.
  // Mirror the recomp exactly: take a guest stack frame, write the angles there, restore on the way out.
  uint32_t saved_sp = c->r[29];
  c->r[29] = saved_sp - 56;                               // recomp's stack frame (private scratch, not 0x1F800xxx)
  c->mem_w32(c->r[29] + 16, (uint32_t)sway0 << 2);
  c->mem_w32(c->r[29] + 20, (uint32_t)sway1 << 2);
  c->mem_w32(c->r[29] + 24, (uint32_t)sway2 << 2);
  c->r[4] = SCR; c->r[5] = c->r[29] + 16; rec_dispatch(c, 0x80084520u);
  c->r[29] = saved_sp;                                    // pop the frame
}

void Render::terrain() {
  Core* c = mCore;
  if (cfg_dbg("terrgte")) fprintf(stderr, "[Render::terrain] node(a0=r4)=%08X\n", c->r[4]);
  // Pick this area's light config ONCE per world frame (terrain renders first); the per-face shader reads
  // the cached pointer. Cheap guest-RAM fingerprint read; unknown area -> village SUN default.
  if (g_mods.light) shadeSelect();
  // Dual-core diff: the `b` core neutralizes terrain to the recomp body via a per-Game flag (the override
  // table is shared; the per-core choice is this flag, not a divergent table). `a` keeps the native path.
  if (c->game->neutralize_terrain) { rec_super_call(c, 0x8002AB5Cu); return; }
  // RENDER PC-NATIVE (USER DIRECTIVE: behave like a PC game, do NOT simulate PSX). The terrain is rendered
  // by NativeScenePass::terrainRender — float transform + real per-pixel depth, drawn straight to the rasterizer, NO GTE
  // compose / NO gte_op / NO byte-packed PSX packet. (The old GTE-compose + 0x80027768-submit transcription
  // oracle was removed — no gating.) prepObjectMatrix does the gameplay sway writes + object-matrix
  // scene data; the render method is PC-native float.
  mNativeScene.terrainRender();
}

// PSXPORT_DEBUG=rwalk — phase-2 render-walk caller counter. The per-object render dispatch
// gen_func_8003CCA4 is driven by one of several orchestrators (the render-layer/list drainers); count
// which fire per scene so the phase-2 flush walk worth owning next is picked by data. Super-calls.
// 0x8003B588 (later-231 "Pass A") — the field WATER render pass, OWNED native real-depth.
// Node 0x800E7E80 (cmd-ptr array @+0xC0). Structure (disas.py 0x8003b588): node-byte bookkeeping
// (anim/state @0x8003b5a0..0x8003b698, ported 1:1 below), then the PSX per-object transform SETUP leaf
// 0x800597AC (rec_dispatch — still PSX, does NO render), then the per-object RENDER. Live, node+0xD=0 →
// (node+0xD)&0xB=0 → render-case table 0x80014EC8[0]=0x8003CD00 = the native eproj FLUSH case, so routing
// the render through the native submit_perobj_render gives the water world-coord FLOAT projection with REAL
// per-vertex depth. Previously the whole pass ran as pure PSX (render_frame.cpp d0(0x8003b588)), so the
// inner jal 0x8003CCA4 emitted GTE packets the native renderer couldn't project (is3d=0) → the water drew
// as a flat 2D FOREGROUND fill OVER the world (the "sea on top" bug). NB: own this TOGETHER with the native
// ground (later-231 caveat) so both sort by real depth — a still-2D-FG ground would occlude the real-depth water.
void Render::rwalkB588() {
  Core* c = mCore;
  const uint32_t node = 0x800E7E80u;
  uint32_t v1 = c->mem_r8(node + 0x0D);
  if ((v1 & 0xD0) == 0) {
    c->mem_w8(node + 0x0D, 0);                                   // @698
  } else {
    v1 |= 0x02; c->mem_w8(node + 0x0D, v1);                      // @5b0/@5bc
    if (!(v1 & 0x20)) {                                          // @5b8: (v1&0x20)==0 → byte setup
      uint32_t g = c->mem_r8(0x1F800247u);                       // @5cc/@608/@658 (same addr)
      if (v1 & 0x10) {                                           // @5c0 → @5cc
        int to5f4 = ((g & 0x30) != 0) || ((g & 0x03) < 2);       // @5d4 / @5e0+@5e4
        if (!to5f4) {                                            // @5e8 → @68c (v0=208)
          c->mem_w8(node + 0x18, 208); c->mem_w8(node + 0x19, 208); c->mem_w8(node + 0x1A, 208);
        } else {                                                 // @5f4
          uint32_t v1b = c->mem_r8(node + 0x0D);
          if (v1b & 0x80) {                                      // @5fc≠0 → @604/@684 (v0 computed, then 32/32)
            int32_t r = ((int32_t)((uint32_t)c->trig.rsin((int32_t)((g & 0x0F) << 7)) << 16)) >> 22;   // FUN_80083E80 -> native Trig::rsin
            c->mem_w8(node + 0x18, (uint8_t)(r + 48)); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
          } else if (v1b & 0x40) {                               // @62c → @634 (v0=32)
            c->mem_w8(node + 0x18, 32); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
          } else {                                               // @640 (v0=128)
            c->mem_w8(node + 0x18, 128); c->mem_w8(node + 0x19, 128); c->mem_w8(node + 0x1A, 128);
          }
        }
      } else {                                                   // @64c: (v1&0x10)==0
        if (v1 & 0x80) {                                         // @650≠0 → @654/@680/@684 (v0=r+16+32, then 32/32)
          int32_t r = ((int32_t)((uint32_t)c->trig.rsin((int32_t)((g & 0x0F) << 7)) << 16)) >> 22;   // FUN_80083E80 -> native Trig::rsin
          c->mem_w8(node + 0x18, (uint8_t)(r + 48)); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
        } else {                                                 // @67c → @680/@684 (v0=0+32)
          c->mem_w8(node + 0x18, 32); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
        }
      }
    }
  }
  c->r[4] = node; rec_dispatch(c, 0x800597ACu);                  // @69c: PSX transform setup (no render)
  if (c->mem_r8(node + 1) != 0) {                                // @6a4: per-object RENDER (native real-depth)
    uint8_t s1 = c->mem_r8(node + 8);
    if ((c->mem_r16(node + 0x17E) & 0x20) && c->mem_r8(node + 0x179)) c->mem_w8(node + 8, c->mem_r8(node + 9));
    c->r[4] = node; perObjRender();                              // was inner jal 0x8003CCA4 (PSX GTE)
    c->mem_w8(node + 8, s1);
  }
}
