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
// (gpu_draw_world_quad). No GTE compose, no gte_op for render, no GP0 packet, no guest write beyond the
// faithful gameplay prep. ov_terrain (engine_submit.cpp) routes here unconditionally — the ONE behavior.
#include "core.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"   // g_mods — engine-native directional lighting on the terrain (matches engine_submit)
#include "render_internal.h"   // sil_bbox_log_node (dark-outline coverage-gap diag)
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// Shared faithful gameplay prep (sway bytes + object rotation matrix @ SCR) — engine_submit.cpp.
void  terrain_prep_object_matrix(Core* c, uint32_t node);
// Native depth normalization (gte_beetle.cpp): map view-Z -> [0,1] for the renderer's D32 buffer.
float proj_pz_to_ord(float pz);
void  proj_set_H(uint16_t h);
// sv = the quad's 4 VIEW-SPACE verts (x,y,z) for the shadow map (NULL = no cast); carried on the queued
// item so it rebuilds per present pass (render_queue.h sh_cast) — no separate shadow stream / keep_shadow.
void  gpu_draw_world_quad(Core* c, const float* px, const float* py, const float* depth,
                          const int* u, const int* v, const uint8_t* r, const uint8_t* g,
                          const uint8_t* b, uint16_t tp, uint16_t clut, int semi,
                          const float (*sv)[3]);
int   gpu_gpu_shadows_active(void);

#define SCR              0x1F800000u          // PSX scratchpad base (engine's GTE-compose temp area)
#define TERRAIN_GEOMBLK  0x8009FAE8u          // terrain prim-record buffer (recomp 0x8002AB5C: lui 0x800A+addiu -1304)

static inline float r16f(Core* c, uint32_t a) { return (float)(int16_t)c->mem_r16(a); }

// gen_func_8002AB5C, rebuilt PC-native. a0(=r4) = the terrain render-list node.
void terrain_render_pc(Core* c) {
  uint32_t node = c->r[4];
  // Faithful gameplay half: write the sway bytes + build the object rotation matrix at SCR (interpreted
  // leaves 0x80085480/0x80084520). This is scene data (the terrain's orientation); native-izing RotMatrix
  // is a later step. After this the object matrix is at SCR, the camera matrix at SCR+0xF8 (set earlier).
  terrain_prep_object_matrix(c, node);

  // ---- read scene data as FLOAT ----------------------------------------------------------------------
  // object rotation matrix @ SCR: element[row][col] = (int16)mem_r16(SCR + col*2 + row*6), /4096.
  float Robj[3][3];
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      Robj[row][col] = r16f(c, SCR + (uint32_t)col*2 + (uint32_t)row*6) / 4096.0f;
  // camera rotation matrix @ SCR+0xF8 (CR-packed, same layout as GTE CR0-4), /4096.
  uint32_t k0 = c->mem_r32(SCR+0xF8), k1 = c->mem_r32(SCR+0xFC), k2 = c->mem_r32(SCR+0x100),
           k3 = c->mem_r32(SCR+0x104), k4 = c->mem_r32(SCR+0x108);
  float Rcam[3][3] = {
    { (int16_t)k0/4096.0f,        (int16_t)(k0>>16)/4096.0f, (int16_t)k1/4096.0f },
    { (int16_t)(k1>>16)/4096.0f,  (int16_t)k2/4096.0f,       (int16_t)(k2>>16)/4096.0f },
    { (int16_t)k3/4096.0f,        (int16_t)(k3>>16)/4096.0f, (int16_t)k4/4096.0f } };
  // camera translation @ SCR+0x10C/110/114 (int32 view units — NOT /4096).
  float camT[3] = { (float)(int32_t)c->mem_r32(SCR+0x10C),
                    (float)(int32_t)c->mem_r32(SCR+0x110),
                    (float)(int32_t)c->mem_r32(SCR+0x114) };
  // Publish the MAIN scene camera view matrix (Rcam + camT) for deterministic per-object world-position
  // depth: it is read here from the scratchpad at terrain-draw time, when it holds the real scene camera
  // (the per-object compose later overwrites the scratchpad with object-specific transforms, so reading it
  // at object-render time is volatile). gpu_native uses this to project each object's WORLD POSITION to a
  // stable view-Z, consistent with the terrain it stands on. See proj_camview_world_ord.
  { void camview_publish(const float R[3][3], const float T[3]); camview_publish(Rcam, camT); }
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

  // projection params (match proj_native_vertex float path, gte_beetle.cpp).
  uint16_t H = (uint16_t)gte_read_ctrl(26);
  float ofx = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;
  float ofy = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
  proj_set_H(H);
  float nearp = (float)H * 0.5f; if (nearp < 1.0f) nearp = 1.0f;

  // ---- iterate the byte-packed terrain records; draw each as one quad ---------------------------------
  // X/Y are signed bytes <<8; Z is the top byte of each RGB word (also <<8). UV/CLUT/texpage/colors decode
  // exactly like the byte-packed submitter (engine_submit.cpp submit_poly_gt4_bp), with a1=a2=a3=0 (no
  // CLUT-bank / OT-Z / U offset for terrain).
  static const uint32_t XO[4] = {0x1C,0x1D,0x20,0x21}, YO[4] = {0x1E,0x1F,0x22,0x23},
                        ZO[4] = {0x0F,0x13,0x17,0x1B}, CO[4] = {0x0C,0x10,0x14,0x18};
  int s_dbg = cfg_dbg("terrpc");
  int drawn = 0;
  for (uint32_t rec = TERRAIN_GEOMBLK; ; rec += 36) {
    int32_t ctl = (int32_t)c->mem_r32(rec + 4);
    float px[4], py[4], depth[4]; int u[4], v[4]; uint8_t r[4], g[4], b[4]; float wv[4][3];
    for (int kk = 0; kk < 4; kk++) {
      float vx = (float)((int)(int8_t)c->mem_r8(rec + XO[kk]) << 8);
      float vy = (float)((int)(int8_t)c->mem_r8(rec + YO[kk]) << 8);
      float vz = (float)((int)(int8_t)c->mem_r8(rec + ZO[kk]) << 8);
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
    // Engine-native directional lighting on the ground (same model as engine_submit.cpp engine_shade_face):
    // per-face view-space normal from the quad's verts, modulate the vertex colours by ambient+diffuse·(N·L).
    if (g_mods.light && !semi) {
      float e1x = wv[1][0]-wv[0][0], e1y = wv[1][1]-wv[0][1], e1z = wv[1][2]-wv[0][2];
      float e2x = wv[2][0]-wv[0][0], e2y = wv[2][1]-wv[0][1], e2z = wv[2][2]-wv[0][2];
      float nx = e1y*e2z-e1z*e2y, ny = e1z*e2x-e1x*e2z, nz = e1x*e2y-e1y*e2x;
      float ln = sqrtf(nx*nx+ny*ny+nz*nz);
      if (ln > 1e-3f) {
        nx/=ln; ny/=ln; nz/=ln; if (nz > 0.0f) { nx=-nx; ny=-ny; nz=-nz; }
        float lx=g_mods.light_dir[0], ly=g_mods.light_dir[1], lz=g_mods.light_dir[2];
        float ll=sqrtf(lx*lx+ly*ly+lz*lz); if (ll>1e-6f){lx/=ll;ly/=ll;lz/=ll;}
        float ndl = nx*lx+ny*ly+nz*lz; if (ndl<0) ndl=0;
        float lit = g_mods.light_ambient + g_mods.light_diffuse*ndl;
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
    gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, cast);
    drawn++;
    if (ctl <= 0) break;                                   // control sign marks the last record
  }
  if (s_dbg) fprintf(stderr, "[terrpc] node=%08x drew %d quads (H=%u ofx=%.1f ofy=%.1f)\n",
                     node, drawn, (unsigned)H, ofx, ofy);
}
