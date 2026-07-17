// PC-NATIVE field terrain renderer (USER DIRECTIVE: render the game PC-native, do NOT transcribe the PSX
// GTE/packet pipeline — see CLAUDE.md "RENDER is the EXCEPTION" + memory render-port-pc-native-...).
//
// This is the first PC-native render target. The OLD "faithful" path was a transcription of the PSX terrain
// submit: it composed the camera × object matrix in the GTE's fixed-point (MVMVA columns + CR-packing),
// drove the emulated GTE per vertex (RTPT/RTPS), and assembled byte-identical GP0 packets — and THAT
// replication is the terrain render bug (water/garbage at the field, journal later-157). That transcription
// oracle has been REMOVED (no gating). Here we instead read the same SCENE DATA the engine already computed
// (camera rotation+translation, the per-object rotation matrix, the object position, the terrain model
// geometry) and render it with FLOAT matrices + real per-pixel depth, straight to the VK rasterizer
// (RenderQueue::drawWorldQuad). No GTE compose, no gte_op for render, no GP0 packet, no guest write beyond the
// faithful gameplay prep. ov_terrain (submit.cpp) routes here unconditionally — the ONE behavior.
#include "core.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"   // g_mods — engine-native directional lighting on the terrain (matches submit.cpp)
#include "render_internal.h"   // sil_bbox_log_node (dark-outline coverage-gap diag)
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// Shared faithful gameplay prep (sway bytes + object rotation matrix @ SCR) — submit.cpp.
// class ProjParams (per-Core) — depth-normalize + set-plane-H + camview publish. Header brings in the
// free-function bridges (proj_pz_to_ord / proj_set_H / camview_publish) used below.
#include "proj_params.h"
// sv = the quad's 4 VIEW-SPACE verts (x,y,z) for the shadow map (NULL = no cast); carried on the queued
// item so it rebuilds per present pass (render_queue.h sh_cast) — no separate shadow stream / keep_shadow.
int   gpu_gpu_shadows_active(void);

#define SCR              0x1F800000u          // PSX scratchpad base (engine's GTE-compose temp area)
#define TERRAIN_GEOMBLK  0x8009FAE8u          // terrain prim-record buffer (recomp 0x8002AB5C: lui 0x800A+addiu -1304)

static inline float r16f(Core* c, uint32_t a) { return (float)c->mem_r16s(a); }

// READ-ONLY terrain matrix build (USER 2026-07-07, issue #32: the display pass may not write guest
// memory or GTE state). Mirrors the guest math bit-exactly with host storage:
//   - rotation matrix from the node's euler angles = Math::rotmat's element math (FUN_80085480,
//     R=Rz·Ry·Rx, same trig LUT reads @0x800a6490, same GPF clamp / >>12 truncation) minus its
//     guest/GTE stores.
//   - sway column-scale = FUN_80084520 (Ghidra 2026-07-07, scratch/decomp/80084520.c): M' = M·diag(f),
//     per-column (f[col]*el)>>12 truncated to int16. f = {sway0<<2, sway1<<2, sway2<<2}; sway0/2
//     computed from the node exactly as the substrate terrain body computes them, sway1 read back
//     from the engine global 0x800A2015 (set elsewhere; the substrate body also reads it back).
// The substrate terrain body (0x8002AB5C, running underneath via the render orchestrator) owns the
// guest-visible writes of this prep (0x800A2014/16 sway bytes, IR0 stage, FarColor); this pass READS.
static inline void trig_lut(Core* c, int32_t angle, int* s, int* co) {
  int32_t sign = angle >> 31;
  uint32_t absa = (uint32_t)((angle + sign) ^ sign);
  uint32_t word = c->mem_r32(0x800a6490u + ((absa << 2) & 0x3ffcu));
  *co = (int)((int32_t)word >> 16);
  uint32_t at = (word << 16); at = (at + (uint32_t)sign) ^ (uint32_t)sign;
  *s = (int)(int16_t)(at >> 16);
}
static inline int16_t gpf1s(int ir0, int ir) {
  int32_t v = ((int32_t)ir0 * ir) >> 12;
  return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}
static void terrain_obj_matrix_host(Core* c, uint32_t node, int16_t m[9]) {
  uint32_t w0 = c->mem_r32(node + 84);
  int sx,cx,sy,cy,sz,cz;
  trig_lut(c, (int16_t)w0,       &sx, &cx);
  trig_lut(c, (int16_t)(w0>>16), &sy, &cy);
  trig_lut(c, c->mem_r16s(node + 88), &sz, &cz);
  int16_t cxsy = gpf1s(cx, sy), cxsz = gpf1s(cx, sz), cxcz = gpf1s(cx, cz);
  int16_t sxsy = gpf1s(sx, sy), sxsz = gpf1s(sx, sz), sxcz = gpf1s(sx, cz);
  int16_t czcy = gpf1s(cz, cy), cz_sxsy = gpf1s(cz, sxsy), cz_cxsy = gpf1s(cz, cxsy);
  int16_t szcy = gpf1s(sz, cy), sz_sxsy = gpf1s(sz, sxsy), sz_cxsy = gpf1s(sz, cxsy);
  int16_t cycx = (int16_t)(((int32_t)cy * cx) >> 12);
  int16_t cysx = (int16_t)(((int32_t)cy * sx) >> 12);
  m[0]=czcy;                      m[1]=(int16_t)-szcy;            m[2]=(int16_t)sy;
  m[3]=(int16_t)(cz_sxsy + cxsz); m[4]=(int16_t)(cxcz - sz_sxsy); m[5]=(int16_t)-cysx;
  m[6]=(int16_t)(sxsz - cz_cxsy); m[7]=(int16_t)(sxcz + sz_cxsy); m[8]=cycx;
  int32_t a80 = c->mem_r16s(node + 80);
  int32_t f[3] = { (int32_t)((uint8_t)((c->mem_r16s(node + 64) * a80) >> 11)) << 2,
                   (int32_t)c->mem_r8(0x800A2015u) << 2,
                   (int32_t)((uint8_t)((c->mem_r16s(node + 66) * a80) >> 11)) << 2 };
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      m[row*3+col] = (int16_t)(((int32_t)m[row*3+col] * f[col]) >> 12);
}

// gen_func_8002AB5C, rebuilt PC-native. a0(=r4) = the terrain render-list node.
void NativeScenePass::terrainRender() {
  Core* c = mCore;
  uint32_t node = c->r[4];
  // ---- read scene data as FLOAT ----------------------------------------------------------------------
  // object rotation matrix — host-computed (read-only), /4096.
  int16_t mobj[9]; terrain_obj_matrix_host(c, node, mobj);
  float Robj[3][3];
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      Robj[row][col] = (float)mobj[row*3+col] / 4096.0f;
  // camera rotation (CR-packed int16 rows) + translation (int32 view units) + projection constants, read
  // through the fps60 provider: byte-identical to the plain scratchpad/CR read when fps60 is off or this
  // is the real per-logic-frame call (which also captures the result into Fps60::mCamCur). During Tier-1's
  // present-time terrain re-render (Fps60::present_vk / Render::terrainRenderAll under mCamOverrideOn), it
  // instead returns the LERPED camera with no guest read — see fps60.cpp.
  float Rc_i16[3][3], camT[3], cam_ofx, cam_ofy, cam_H;
  c->game->fps60.sceneCam(c, Rc_i16, camT, cam_ofx, cam_ofy, cam_H);
  float Rcam[3][3];
  for (int rr = 0; rr < 3; rr++) for (int cc = 0; cc < 3; cc++) Rcam[rr][cc] = Rc_i16[rr][cc] / 4096.0f;
  // Publish the MAIN scene camera view matrix (Rcam + camT) for deterministic per-object world-position
  // depth: it is read here from the scratchpad at terrain-draw time, when it holds the real scene camera
  // (the per-object compose later overwrites the scratchpad with object-specific transforms, so reading it
  // at object-render time is volatile). gpu_native uses this to project each object's WORLD POSITION to a
  // stable view-Z, consistent with the terrain it stands on. See proj_camview_world_ord.
  camview_publish(Rcam, camT);
  // object position @ node+72 (x lo / y hi) and node+76 (z lo).
  uint32_t p72 = c->mem_r32(node+72);
  float objP[3] = { (float)(int16_t)p72, (float)(int16_t)(p72>>16), (float)(int16_t)c->mem_r32(node+76) };

  // R_view = R_cam · R_obj ;  T_view = R_cam · objP + camT.  (The GTE MVMVA does (M·v)>>12 = M_float·v,
  // so composing the /4096 float matrices reproduces the fixed-point compose's magnitude exactly.)
  float Rview[3][3], Tview[3];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      float s = 0; for (int k = 0; k < 3; k++) s += Rcam[i][k] * Robj[k][j]; Rview[i][j] = s;
    }
    float t = camT[i]; for (int k = 0; k < 3; k++) t += Rcam[i][k] * objP[k]; Tview[i] = t;
  }

  // projection params (from the fps60 provider above — interpolated during the mid-present).
  uint16_t H = (uint16_t)cam_H;
  float ofx = cam_ofx;
  float ofy = cam_ofy;
  proj_set_H(H);
  float nearp = (float)H * 0.5f; if (nearp < 1.0f) nearp = 1.0f;

  // ---- iterate the byte-packed terrain records; draw each as one quad ---------------------------------
  // X/Y are signed bytes <<8; Z is the top byte of each RGB word (also <<8). UV/CLUT/texpage/colors decode
  // exactly like the byte-packed submitter (submit.cpp submit_poly_gt4_bp), with a1=a2=a3=0 (no
  // CLUT-bank / OT-Z / U offset for terrain).
  static const uint32_t XO[4] = {0x1C,0x1D,0x20,0x21}, YO[4] = {0x1E,0x1F,0x22,0x23},
                        ZO[4] = {0x0F,0x13,0x17,0x1B}, CO[4] = {0x0C,0x10,0x14,0x18};
  int s_dbg = cfg_dbg("terrpc");
  int drawn = 0;
  // Tag every terrain quad with the reserved kTerrainDbgNode sentinel (render_queue.h) — NOT node==0 the
  // way un-owned prims default. fieldEntityRender (the SOP field-overlay SCENE TABLE walk: grass/props)
  // is the OTHER dbg_node==0 RQ_WORLD producer and is genuinely un-owned (still queue-lerped); tagging
  // terrain distinctly is what lets Fps60::tier1Render's queue-lerp exclusion (fps60.cpp isTier1Owned)
  // target ONLY the prims it actually re-renders.
  c->rsub.diag.beginObject(kTerrainDbgNode);
  for (uint32_t rec = TERRAIN_GEOMBLK; ; rec += 36) {
    int32_t ctl = (int32_t)c->mem_r32(rec + 4);
    float px[4], py[4], depth[4]; int u[4], v[4]; uint8_t r[4], g[4], b[4]; float wv[4][3];
    for (int kk = 0; kk < 4; kk++) {
      float vx = (float)((int)c->mem_r8s(rec + XO[kk]) << 8);
      float vy = (float)((int)c->mem_r8s(rec + YO[kk]) << 8);
      float vz = (float)((int)c->mem_r8s(rec + ZO[kk]) << 8);
      float w0 = Rview[0][0]*vx + Rview[0][1]*vy + Rview[0][2]*vz + Tview[0];
      float w1 = Rview[1][0]*vx + Rview[1][1]*vy + Rview[1][2]*vz + Tview[1];
      float w2 = Rview[2][0]*vx + Rview[2][1]*vy + Rview[2][2]*vz + Tview[2];
      wv[kk][0] = w0; wv[kk][1] = w1; wv[kk][2] = w2;        // view-space pos (for the per-face light normal)
      float pz = w2; if (pz < nearp) pz = nearp;
      float ph = (float)H / pz;
      float sx = ofx + w0 * ph; if (sx < -1024.f) sx = -1024.f; if (sx > 1023.f) sx = 1023.f;
      float sy = ofy + w1 * ph; if (sy < -1024.f) sy = -1024.f; if (sy > 1023.f) sy = 1023.f;
      px[kk] = sx; py[kk] = sy; depth[kk] = proj_pz_to_ord(pz);
      uint32_t col = c->mem_r32(rec + CO[kk]);             // raw RGB (top byte = Z, ignored). FIRST CUT:
      r[kk] = col & 0xFF; g[kk] = (col >> 8) & 0xFF; b[kk] = (col >> 16) & 0xFF;  // no DPCT/DPCS depth-cue.
    }
    // UVs: v0 from rec+0 (+ CLUT high half); v1 from ctl low half (+ texpage high half); v2/v3 from rec+8.
    uint32_t uv0 = c->mem_r32(rec + 0);
    uint16_t clut = (uint16_t)((uv0 >> 16) & 0xFFFF);
    uint16_t tp   = (uint16_t)(((uint32_t)ctl & 0x7FFFFFu) >> 16);   // = packet uv1|tpage high half
    uint32_t uv2  = c->mem_r32(rec + 8);
    u[0] = uv0 & 0xFF;        v[0] = (uv0 >> 8) & 0xFF;
    u[1] = (uint32_t)ctl & 0xFF; v[1] = ((uint32_t)ctl >> 8) & 0xFF;
    u[2] = uv2 & 0xFF;        v[2] = (uv2 >> 8) & 0xFF;
    u[3] = (uv2 >> 16) & 0xFF; v[3] = (uv2 >> 24) & 0xFF;
    int semi = (ctl & 0x40000000) ? 1 : 0;
    // Engine-native directional lighting on the ground (same model as submit.cpp engine_shade_face):
    // per-face view-space normal from the quad's verts, modulate the vertex colours by ambient+diffuse·(N·L).
    const Mods& mm = c->game->mods;
    if (mm.light && !semi) {
      float e1x = wv[1][0]-wv[0][0], e1y = wv[1][1]-wv[0][1], e1z = wv[1][2]-wv[0][2];
      float e2x = wv[2][0]-wv[0][0], e2y = wv[2][1]-wv[0][1], e2z = wv[2][2]-wv[0][2];
      float nx = e1y*e2z-e1z*e2y, ny = e1z*e2x-e1x*e2z, nz = e1x*e2y-e1y*e2x;
      float ln = sqrtf(nx*nx+ny*ny+nz*nz);
      if (ln > 1e-3f) {
        nx/=ln; ny/=ln; nz/=ln; if (nz > 0.0f) { nx=-nx; ny=-ny; nz=-nz; }
        float lx=mm.light_dir[0], ly=mm.light_dir[1], lz=mm.light_dir[2];
        float ll=sqrtf(lx*lx+ly*ly+lz*lz); if (ll>1e-6f){lx/=ll;ly/=ll;lz/=ll;}
        float ndl = nx*lx+ny*ly+nz*lz; if (ndl<0) ndl=0;
        float lit = mm.light_ambient + mm.light_diffuse*ndl;
        for (int kk=0; kk<4; kk++) {
          int rr=(int)(r[kk]*lit+0.5f), gg=(int)(g[kk]*lit+0.5f), bb=(int)(b[kk]*lit+0.5f);
          r[kk]=(uint8_t)(rr>255?255:rr); g[kk]=(uint8_t)(gg>255?255:gg); b[kk]=(uint8_t)(bb>255?255:bb);
        }
      }
    }
    if (s_dbg && drawn < 4)
      fprintf(stderr, "[terrpc] rec=%08x v0=(%.1f,%.1f z%.3f) v2=(%.1f,%.1f) tp=%04x clut=%04x semi=%d\n",
              rec, px[0], py[0], depth[0], px[2], py[2], tp, clut, semi);
    // Dynamic shadow: the terrain quad's view-space verts (x=w0, y=w1, z=pz) so the ground both CASTS into
    // the shadow map (self-occlusion across hills) and RECEIVES shadows. Carried on the queued item (sv) so
    // it rebuilds per present pass from the queue — no separate shadow stream, no keep_shadow.
    float sv[4][3]; const float (*cast)[3] = nullptr;
    if (!semi && gpu_gpu_shadows_active()) {
      for (int kk = 0; kk < 4; kk++) { float pz = wv[kk][2]; if (pz < nearp) pz = nearp;
        sv[kk][0] = wv[kk][0]; sv[kk][1] = wv[kk][1]; sv[kk][2] = pz; }
      cast = sv;
    }
    sil_bbox_log_verts("terrain", px, py, depth, 4, node, rec, r, g, b);
    // Tier-1 capture-target redirect (game.h Game::rqRedirect): the ISOLATED sink while Fps60::present_vk
    // re-runs this pass under a lerped camera at the interp present; the live queue otherwise.
    RenderQueue& rqOut = c->game->rqRedirect ? *c->game->rqRedirect : c->game->rq;
    rqOut.drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, cast);
    drawn++;
    if (ctl <= 0) break;                                   // control sign marks the last record
  }
  c->rsub.diag.endObject();
  if (s_dbg) fprintf(stderr, "[terrpc] node=%08x drew %d quads (H=%u ofx=%.1f ofy=%.1f)\n",
                     node, drawn, (unsigned)H, ofx, ofy);
}
