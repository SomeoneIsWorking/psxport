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
#include "game_ctx.h"
#include "game.h"   // Fps60::current_object (was g_current_object)
#include "cfg.h"
#include "mods.h"   // Mods (game->mods) — live PC-native lighting params (engine-native shading, not a deferred pass)
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
int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
static int submit_xmax(Core* c) { return gpu_gpu_wide_engine(c) ? gpu_gpu_wide_engine_w(c) : 320; }

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
// g_fps60_on retired — read game->mods.fps60 (mods.h)
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
// fps60 (docs/fps60-rework.md, redesign 2026-07-14): the 60fps in-between is built from the resolved
// render-queue prims (RqItem, matched + lerped by Fps60::matchAndLerp), not from a per-quad model-vertex/
// transform stamp here — nothing in this submit path needs to tag for fps60. See game/render/fps60.cpp.

// ENGINE-NATIVE directional lighting (user directive 2026-06-21: lighting must be engine-native, NOT a
// screen-space deferred pass). Compute a real per-FACE normal from the prim's own view-space geometry
// (cross of two edges of ProjVtx.vx/vy/vz = the GTE-rotated vertex = view-space position) and modulate the
// vertex colours by ambient + diffuse*max(0,N·L). This shades ONLY the opaque world geometry it is called
// on — it never touches semi-transparent surfaces (water etc.), so translucency is unaffected by
// construction (that was the deferred pass's bug: it re-shaded/clobbered pixels behind translucent water).
// Light dir is the to-light vector in view space (mods.light_dir), same convention as the retired pass.
// PER-AREA lighting (engine/lighting.cpp): the directional light is now COLOURED and AREA-SELECTED (open
// areas get a warm SUN, mines get a dim cave ambient), plus optional POINT lights (lava up-glow / torches)
// attenuated by the face's view-space position. Config picked once per frame in Render::shadeSelect() (the
// renderer caches it so this hot per-face routine doesn't re-read guest RAM). lit is now per-CHANNEL: each
// vertex colour is modulated by (ambient_col*ambient + dir_col*diffuse*N·L + Σ point_col*att*N·L).
void Render::shadeSelect() {                 // called once per world frame before the submitters run
  mShadeCfg = lighting.select(lighting.areaKeyFrom(mCore));
}
static inline void engine_shade_face(Core* c, const ProjVtx* p, int nv, uint8_t r[4], uint8_t g[4], uint8_t b[4]) {
  if (!c->game->mods.light) return;
  Render* rr = rend(c);
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
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) cfg_logf("subc", "gt3_native %ld", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  // H for depth normalization comes from the ACTIVE xform (mActiveXform.H), not a raw CR26 read: every
  // caller sets an active xform before reaching here (projSetActive — no GTE fallback, see render.h), and
  // that xform's H is the SAME camera H projVertexActive already used for this vertex's screen XY (real
  // frame: read via sceneCam; Tier-1 present-time re-render: the LERPED camera, sceneCam's mCamOverrideOn
  // branch). A raw gte_read_ctrl(26) read here would (a) be a present-time GUEST READ — the fps60
  // present-time invariant forbids that — and (b) desync depth normalization from the lerped H used for
  // XY at any t other than 1 (found while extending Tier 1 to fieldEntityRender, docs/fps60-rework.md).
  proj_set_H((uint16_t)rend(c)->mActiveXform.H);
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    uint32_t vz01 = c->mem_r32(rec + 20);
    uint32_t xy0 = c->mem_r32(rec + 16), xy1 = c->mem_r32(rec + 24), xy2 = c->mem_r32(rec + 28);
    ProjVtx p[3];
    rend(c)->projVertexActive( (int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,         &p[0]);
    rend(c)->projVertexActive( (int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16), &p[1]);
    rend(c)->projVertexActive( (int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)c->mem_r32(rec + 32), &p[2]);
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface
    int xmax = submit_xmax(c);
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
    if (cfg_dbg("eprojv")) {
      static long objn = -1; uint32_t geomblk = c->rsub.diag.currentGeomblk();
      if ((long)geomblk != objn) { objn = (long)geomblk;
        const EObjXform& x = rend(c)->mActiveXform;
        cfg_logf("eprojv", "cmd=%08x R=[%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f] T=(%.2f,%.2f,%.2f) H=%.0f",
          geomblk, (double)x.R[0][0],(double)x.R[0][1],(double)x.R[0][2],
                   (double)x.R[1][0],(double)x.R[1][1],(double)x.R[1][2],
                   (double)x.R[2][0],(double)x.R[2][1],(double)x.R[2][2],
                   (double)x.T[0],(double)x.T[1],(double)x.T[2],(double)x.H); }
      fprintf(stderr, "[eprojv] cmd=%08x gt3 idx=%u v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) v2=(%.2f,%.2f,%.2f)\n",
        geomblk, i, (double)px[0],(double)py[0],(double)depth[0], (double)px[1],(double)py[1],(double)depth[1],
        (double)px[2],(double)py[2],(double)depth[2]);
    }
    { char tag[32]; snprintf(tag, sizeof tag, "gt3_native@%08X", c->rsub.diag.currentGeomblk()); sil_bbox_log_verts(tag, px, py, depth, 3, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 3, semi, vv);   // dynamic shadow verts (carried on the item)
      // Tier-1 capture-target redirect (game.h Game::rqRedirect), same as native_terrain.cpp: the ISOLATED
      // sink while Fps60::tier1Render re-runs fieldEntityRender under a lerped camera; the live queue
      // otherwise. Without this, a present-time re-render would corrupt the live queue the NEXT real frame
      // is about to build — the exact bug class the "one draw path" design forbids.
      RenderQueue& rqOut = c->game->rqRedirect ? *c->game->rqRedirect : c->game->rq;
      rqOut.drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
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
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) cfg_logf("subc", "gt4_native %ld", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  // See submitPolyGt3Native above: H comes from the active xform, not a raw present-time CR26 read.
  proj_set_H((uint16_t)rend(c)->mActiveXform.H);
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    // model verts: V0=rec+20(XY)|rec+24.lo(Z), V1=rec+28|rec+24.hi, V2=rec+32|rec+36.lo, V3=rec+40|rec+36.hi
    uint32_t vz01 = c->mem_r32(rec + 24), vz23 = c->mem_r32(rec + 36);
    uint32_t xy0 = c->mem_r32(rec + 20), xy1 = c->mem_r32(rec + 28),
             xy2 = c->mem_r32(rec + 32), xy3 = c->mem_r32(rec + 40);
    ProjVtx p[4];
    rend(c)->projVertexActive( (int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,          &p[0]);
    rend(c)->projVertexActive( (int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16),  &p[1]);
    rend(c)->projVertexActive( (int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)vz23,          &p[2]);
    rend(c)->projVertexActive( (int16_t)xy3, (int16_t)(xy3 >> 16), (int16_t)(vz23 >> 16),  &p[3]);
    // backface cull on the FRONT triangle's signed screen area (NCLIP: (SX1-SX0)*(SY2-SY0)-(SX2-SX0)*(SY1-SY0)).
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface (matches MAC0<=0 drop)
    // frustum cull (right/bottom edges only, as the original) over all 4 verts.
    int xmax = submit_xmax(c);
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
    if (cfg_dbg("eprojv")) {
      uint32_t geomblk = c->rsub.diag.currentGeomblk();
      cfg_logf("eprojv", "cmd=%08x gt4 idx=%u v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) v2=(%.2f,%.2f,%.2f) v3=(%.2f,%.2f,%.2f)",
        geomblk, i, (double)px[0],(double)py[0],(double)depth[0], (double)px[1],(double)py[1],(double)depth[1],
        (double)px[2],(double)py[2],(double)depth[2], (double)px[3],(double)py[3],(double)depth[3]);
    }
    { char tag[32]; snprintf(tag, sizeof tag, "gt4_native@%08X", c->rsub.diag.currentGeomblk()); sil_bbox_log_verts(tag, px, py, depth, 4, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 4, semi, vv);   // dynamic shadow verts (carried on the item)
      // Tier-1 capture-target redirect — see submitPolyGt3Native above.
      RenderQueue& rqOut = c->game->rqRedirect ? *c->game->rqRedirect : c->game->rq;
      rqOut.drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
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
  c->rsub.diag.setGeomblk(geomblk);
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
  EObjXform w; rend(c)->projComposeCamera(&w); rend(c)->projSetActive(&w);
  // Tag every scene-table prim with the reserved kSceneTableDbgNode sentinel (render_queue.h), the same
  // way native_terrain.cpp tags terrain with kTerrainDbgNode: this pass is camera-only (projComposeCamera
  // above — no per-object transform), so it is Tier-1-eligible (docs/fps60-rework.md "Tier 1 landed" +
  // its extension to fieldEntityRender). Both the real per-logic-frame call (render_walk.cpp sceneNative)
  // and Fps60::tier1Render's present-time re-render go through THIS function, so both tag identically —
  // scoped here (not at each call site) so the tag can never be forgotten at one of the two call sites.
  c->rsub.diag.beginObject(kSceneTableDbgNode);
  // DIAG groundproj: log the camera xform + first GT4 record's model verts and their eproj projection, so we
  // can see whether the world-space scene-table geometry projects on-screen with sane depth. (later-231b)
  if (cfg_dbg("groundproj")) { static int n=0; if (n++ < 3) {
    cfg_logf("groundproj", "es=%08x count=%u base=%08x T=(%.0f,%.0f,%.0f) H=%.0f R0=(%.3f,%.3f,%.3f)",
      es, count, base, (double)w.T[0],(double)w.T[1],(double)w.T[2],(double)w.H,
      (double)w.R[0][0]/4096,(double)w.R[0][1]/4096,(double)w.R[0][2]/4096);
    uint32_t cmd0 = base + (uint32_t)c->mem_r16(es+0x10)*4; uint32_t s0d=c->mem_r32(cmd0);
    uint32_t rec = cmd0+4 + ((s0d&0xFF)*36);   // skip GT3s to first GT4 record (44B)
    for (int k=0;k<2;k++){ uint32_t r2=rec+k*44;
      int16_t vx=c->mem_r16s(r2+20), vy=(int16_t)(c->mem_r32(r2+20)>>16), vz=c->mem_r16s(r2+24);
      ProjVtx pv; rend(c)->projVertexActive( vx,vy,vz,&pv);
      cfg_logf("groundproj", "   gt4[%d] model=(%d,%d,%d) -> px=%.1f py=%.1f pz=%.1f sx=%d sy=%d",
        k, vx,vy,vz, (double)pv.px,(double)pv.py,(double)pv.pz, pv.sx, pv.sy); } } }
  uint32_t p = es + 0x10, end = es + 0x10 + (uint32_t)count * 2;
  for (; p < end; p += 2) {
    uint32_t cmd = base + (uint32_t)c->mem_r16(p) * 4;
    uint32_t s0  = c->mem_r32(cmd);
    // fps60: scene-table entities are world-space (projComposeCamera, camera-only) — they interpolate via
    // the camera during the mid-present, no per-object transform key needed.
    c->rsub.diag.setGeomblk(cmd);   // sil_bbox_log diag: tag this entity's cmd record (Render::gt3gt4 is NOT the caller here)
    c->r[4] = cmd + 4;  c->r[5] = otbase; c->r[6] = s0 & 0xFF;          submitPolyGt3Native(c);
    c->r[4] = c->r[2];  c->r[5] = otbase; c->r[6] = (s0 >> 16) & 0xFF;  submitPolyGt4Native(c);
  }
  c->rsub.diag.endObject();
  rend(c)->projClearActive();
}

// ov_ground_probe (diagnostic, `debug groundprobe`) moved to render_debug_probes.cpp (2026-07 restructure).

// RETIRED 2026-07-07 (issue #32): Render::prepObjectMatrix (guest sway/IR0/GTE writes + the
// 0x80084520 dispatch) and Render::rwalkB588 (the water-pass lift: node bookkeeping writes + the
// 0x800597AC transform-setup dispatch — the SBS f26 writer). The substrate walk executes both
// underneath via the render orchestrator; the display pass computes its matrices in host memory
// (native_terrain.cpp terrain_obj_matrix_host). History: this file @ commit 7989159.
void Render::terrain() {
  Core* c = mCore;
  cfg_logf("terrgte", "[Render::terrain] node(a0=r4)=%08X", c->r[4]);
  // Pick this area's light config ONCE per world frame (terrain renders first); the per-face shader reads
  // the cached pointer. Cheap guest-RAM fingerprint read; unknown area -> village SUN default.
  if (c->game->mods.light) shadeSelect();
  // READ-ONLY display pass (issue #32): float transform + real per-pixel depth, matrices computed in
  // host memory (native_terrain.cpp terrain_obj_matrix_host). The substrate terrain body 0x8002AB5C
  // executes underneath via the render orchestrator and owns all guest writes (sway bytes, IR0, GTE).
  mNativeScene.terrainRender();
}

// terrainRenderAll: the terrain-node enumeration (moved from render_walk.cpp's sceneNative so Fps60's
// Tier-1 present-time re-render — docs/fps60-rework.md — can run the EXACT same scan+call sequence, not
// a hand-duplicated copy). Pure reads: the node scan is the same enumeration the substrate walk performs
// (see terrain()'s own header comment); `cfg_dbg("noterr")` stays honored so the existing debug knob keeps
// working from both call sites.
void Render::terrainRenderAll() {
  Core* c = mCore;
  for (uint32_t n = c->mem_r32(0x800F2624u), g = 0; n && g < 400; g++, n = c->mem_r32(n + 36)) {
    if (c->mem_r8(n + 1) == 0) continue;
    if (c->mem_r32(n + 24) == 0x8002AB5Cu && !cfg_dbg("noterr")) {
      c->game->ffspan.begin(); c->r[4] = n; terrain(); c->game->ffspan.end("rwT_terrain");
    }
  }
}
