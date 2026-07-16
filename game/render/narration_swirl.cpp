// game/render/narration_swirl.cpp — native producer for the SOP intro-narration SWIRL/VORTEX.
//
// The narration's full-screen swirl is NOT a cmd-list object: its render node (0x800ED9E8, type 0x20)
// carries a CUSTOM render fn at node+0x18 = 0x8010BF54 (SOP overlay), dispatched by the substrate
// master walk's default case. The native object walk (Render::fieldObjectsRender) used to skip it
// (cmd counts 0/0) — that was the "void beat renders ~black" gap (findings/render.md '#5').
//
// RE (2026-07-16, Ghidra on the void-beat dump — scratch/bin/ram_void.bin):
//   FUN_8010BF54(node):                                  // the swirl render fn
//     IR0(spad 0x1f800090) = 0                           // depth-cue OFF -> colors pass through
//     for blade in {0,1}:
//       FUN_80031D24(node+0x2c, DAT_80108FF0, node+0x48) // transform setup (below)
//       FUN_80027768(&MESH 0x8010CC08, 0, 0, node[0x50]) // mesh submit, U-scroll = node[0x50]
//       node[0x4e] += 0x800                              // second blade rotated +0x800
//   FUN_80031D24 = object matrix: M = colScale( rotmat(node+0x48 angles) · rotY(node[0x4e]),
//                  DAT_80108FF0 bytes<<2 = (128,256,128) ) composed with the CAMERA (spad f8..114) —
//                  exactly what projComposeObjectHost does natively. Translation = SVECTOR node+0x2c.
//   FUN_80027768 = 36-byte quad records at 0x8010CC08 (loop while word1 > 0; the record with word1
//     bit31 set is drawn LAST — its uv1 masks the bit off):
//       word0: u0,v0 bytes + CLUT<<16            word1: u1,v1 bytes + tpage bits16-22; bit30=semi
//       word2: u2,v2 + u3,v3                     words3-6: RGB0..RGB3 (byte3 of each = vertex Z, s8)
//       bytes28-35: x0,y0? no — x0,x1? layout:   x0@28 y0@30 x1@29 y1@31 x2@32 y2@34 x3@33 y3@35
//       vertex k = (s8 x, s8 y, s8 z) * 256 model units. Prim = 0x3C/0x3E gouraud-TEXTURED QUAD.
//     NO backface cull (only the GTE FLAG overflow check); IR0=0 -> DPCT leaves colors unchanged;
//     every U byte gets +node[0x50] (the animated texture scroll; FUN_8010BEAC ticks it -9 wrap +75).
//
// pc_render rules: READ-ONLY (the substrate render fn still runs underneath and owns the node[0x4e]
// advance + spad writes — this producer only reads); picture from game state with real depth.
#include "core.h"
#include "game.h"
#include "render.h"
#include "render_queue.h"
#include "proj_params.h"

namespace {

// Guest sin/cos table lookup (read-only): same table + packing Math::rotmat uses (0x800A6490,
// word = cos<<16 | sin, index = (abs(angle)&0xfff)*4, sin sign-corrected by the angle sign).
inline void trig(Core* c, int32_t angle, int* s, int* co) {
  int32_t sign = angle >> 31;
  uint32_t absa = (uint32_t)((angle + sign) ^ sign);
  uint32_t word = c->mem_r32(0x800a6490u + ((absa << 2) & 0x3ffcu));
  *co = (int)((int32_t)word >> 16);
  uint32_t at = (word << 16); at = (at + (uint32_t)sign) ^ (uint32_t)sign;
  *s = (int)(int16_t)(at >> 16);
}

// rotmat(angles) — host mirror of Math::rotmat's element math (gte_math.cpp), no guest writes.
void rotmatHost(Core* c, int16_t ax, int16_t ay, int16_t az, int32_t M[3][3]) {
  int sx,cx,sy,cy,sz,cz;
  trig(c, ax, &sx, &cx); trig(c, ay, &sy, &cy); trig(c, az, &sz, &cz);
  auto gpf = [](int a, int b) -> int16_t { int32_t v = ((int32_t)a * b) >> 12;
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); };
  int16_t cxsy=gpf(cx,sy), cxsz=gpf(cx,sz), cxcz=gpf(cx,cz);
  int16_t sxsy=gpf(sx,sy), sxsz=gpf(sx,sz), sxcz=gpf(sx,cz);
  int16_t czcy=gpf(cz,cy), cz_sxsy=gpf(cz,sxsy), cz_cxsy=gpf(cz,cxsy);
  int16_t szcy=gpf(sz,cy), sz_sxsy=gpf(sz,sxsy), sz_cxsy=gpf(sz,cxsy);
  int16_t cycx=(int16_t)(((int32_t)cy*cx)>>12), cysx=(int16_t)(((int32_t)cy*sx)>>12);
  M[0][0]=czcy;                    M[0][1]=(int16_t)-szcy;            M[0][2]=sy;
  M[1][0]=(int16_t)(cz_sxsy+cxsz); M[1][1]=(int16_t)(cxcz-sz_sxsy);   M[1][2]=(int16_t)-cysx;
  M[2][0]=(int16_t)(sxsz-cz_cxsy); M[2][1]=(int16_t)(sxcz+sz_cxsy);   M[2][2]=cycx;
}

// rotY(angle) on identity — host mirror of Math::rotY (rotpair rows 0/2, sign -1).
void rotYHost(Core* c, int16_t angle, int32_t M[3][3]) {
  int s,co; trig(c, angle, &s, &co); s = -s;                      // rotY's sign convention (rotpair sgn=-1)
  M[0][0]=co;  M[0][1]=0;    M[0][2]=(int16_t)-s;
  M[1][0]=0;   M[1][1]=4096; M[1][2]=0;
  M[2][0]=s;   M[2][1]=0;    M[2][2]=co;
}

}  // namespace

// narrationSwirlRender — draw the SOP swirl node natively (see banner). `node` is the type-0x20 render
// node whose custom render fn is the SOP swirl renderer; `yawBias` selects the blade (the substrate
// render fn advanced node[0x4e] by +0x800 per blade during exec, so blade k of THIS frame used
// yaw = node[0x4e] - 0x1000 + k*0x800).
void Render::narrationSwirlRender(uint32_t node) {
  Core* c = mCore;
  const uint32_t MESH = 0x8010CC08u;                      // SOP overlay swirl mesh (RE'd constant of 0x8010BF54)
  // transform inputs (all read-only)
  const int16_t ax = (int16_t)c->mem_r16(node + 0x48);
  const int16_t ay = (int16_t)c->mem_r16(node + 0x4a);
  const int16_t az = (int16_t)c->mem_r16(node + 0x4c);
  const int16_t yawNow = (int16_t)c->mem_r16(node + 0x4e);  // post-exec value (both blades already added)
  const uint8_t uScroll = c->mem_r8(node + 0x50);           // animated texture U scroll
  const uint32_t posw0 = c->mem_r32(node + 0x2c), posw1 = c->mem_r32(node + 0x30);
  const float Tobj[3] = { (float)(int16_t)posw0, (float)(int16_t)(posw0 >> 16), (float)(int16_t)posw1 };
  const uint32_t scw = c->mem_r32(0x80108ff0u);             // base col-scale bytes (<<2): (32,64,32)<<2
  const int32_t colScale[3] = { (int32_t)((scw & 0xFF) << 2), (int32_t)(((scw >> 8) & 0xFF) << 2),
                                (int32_t)(((scw >> 16) & 0xFF) << 2) };

  for (int blade = 0; blade < 2; blade++) {
    // Robj = colScale( rotmat(ax,ay,az) · rotY(yaw) )  — integer 1.3.12 math mirroring the guest chain.
    // Blade phase: the substrate advanced node[0x4e] by +0x800 per blade during exec, so this frame's
    // blades were yaw-0x1000 and yaw-0x800. (The pair {yaw-0x800, yaw} renders pixel-identically — the
    // two blades are 0x800-symmetric — verified by pixel-diff; either anchoring is the same picture.)
    const int16_t yaw = (int16_t)(yawNow - 0x1000 + blade * 0x800);
    int32_t M1[3][3], M2[3][3];
    rotmatHost(c, ax, ay, az, M1);
    rotYHost(c, yaw, M2);
    float Robj[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) {
        int32_t s = (int32_t)((((int64_t)M1[i][0]*M2[0][j] + (int64_t)M1[i][1]*M2[1][j] +
                                (int64_t)M1[i][2]*M2[2][j]) >> 12));
        if (s < -32768) s = -32768; else if (s > 32767) s = 32767;   // MVMVA IR clamp (func_80084250)
        Robj[i][j] = (float)((s * colScale[j]) >> 12);               // column scale (FUN_80084520)
      }
    EObjXform w; projComposeObjectHost(Robj, Tobj, &w);
    projSetActive(&w);

    // mesh records (36 B): draw each; the record with word1 bit31 set is the LAST one drawn.
    for (uint32_t rec = MESH; rec < MESH + 36u * 512u; rec += 36u) {
      const uint32_t w0 = c->mem_r32(rec + 0), w1 = c->mem_r32(rec + 4), w2 = c->mem_r32(rec + 8);
      // vertices: s8 * 256 model coords (x0@28 x1@29 y0@30 y1@31 x2@32 x3@33 y2@34 y3@35; z = byte3 of
      // each color word: z0@15 z1@19 z2@23 z3@27).
      const int vx[4] = { (int8_t)c->mem_r8(rec+28)*256, (int8_t)c->mem_r8(rec+29)*256,
                          (int8_t)c->mem_r8(rec+32)*256, (int8_t)c->mem_r8(rec+33)*256 };
      const int vy[4] = { (int8_t)c->mem_r8(rec+30)*256, (int8_t)c->mem_r8(rec+31)*256,
                          (int8_t)c->mem_r8(rec+34)*256, (int8_t)c->mem_r8(rec+35)*256 };
      const int vz[4] = { (int8_t)c->mem_r8(rec+15)*256, (int8_t)c->mem_r8(rec+19)*256,
                          (int8_t)c->mem_r8(rec+23)*256, (int8_t)c->mem_r8(rec+27)*256 };
      float px[4], py[4], depth[4];
      for (int k = 0; k < 4; k++) {
        ProjVtx p; projVertexActive(vx[k], vy[k], vz[k], &p);
        px[k] = p.px; py[k] = p.py; depth[k] = proj_pz_to_ord(p.pz);
      }
      // colors: RGB bytes of words 3..6 (IR0=0 -> depth-cue is a pass-through; raw colors).
      uint8_t r[4], g[4], b[4];
      for (int k = 0; k < 4; k++) {
        const uint32_t cw = c->mem_r32(rec + 12u + (uint32_t)k * 4u);
        r[k] = cw & 0xFF; g[k] = (cw >> 8) & 0xFF; b[k] = (cw >> 16) & 0xFF;
      }
      // UVs (+ the animated U scroll the guest adds to every U byte, u8 wrap) / clut / tpage / semi.
      int u[4] = { (int)((uint8_t)(( w0        & 0xFF) + uScroll)), (int)((uint8_t)(( w1        & 0xFF) + uScroll)),
                   (int)((uint8_t)(( w2        & 0xFF) + uScroll)), (int)((uint8_t)(((w2 >> 16) & 0xFF) + uScroll)) };
      int v[4] = { (int)((w0 >> 8) & 0xFF), (int)((w1 >> 8) & 0xFF),
                   (int)((w2 >> 8) & 0xFF), (int)((w2 >> 24) & 0xFF) };
      const uint16_t clut = (uint16_t)(w0 >> 16);
      const uint16_t tp   = (uint16_t)((w1 >> 16) & 0x7F);   // packet tpage = uv1 word bits16-22 (mode 0)
      const int semi = (w1 & 0x40000000u) ? 1 : 0;
      // NO backface cull (the guest draws both faces — only a GTE overflow FLAG check guards this mesh).
      c->game->activeRq().drawWorldQuad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, nullptr);
      if ((int32_t)w1 <= 0) break;                          // bit31 record = the last one (drawn above)
    }
    projClearActive();
  }
}
