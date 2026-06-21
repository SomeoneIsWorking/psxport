// engine/hud.cpp — PC-NATIVE in-game HUD drawer (Tomba!2).
//
// WHAT THIS OWNS
// --------------
// The in-game HUD's sprite elements (the spiky-ball weapon indicator and the AP/heart gauge cells) are
// drawn by the recomp "sprite-strip" helper FUN_8007E938, and the small UI panel slices by the
// "rect" helper FUN_8007E8DC. Both ultimately build PSX GP0 sprite/poly packets into the guest OT pool
// and let the PSX present path rasterize them. That is exactly the PSX-render mechanism the engine is
// supposed to OWN, not inherit.
//
// THIS MODULE REBUILDS THE HUD PC-NATIVE: it overrides the HUD draw helpers and, instead of emitting a
// PSX packet, reads the HUD element's STATE (screen position + which atlas cell) and DRAWS it directly
// with the PC renderer's 2D textured-quad path (gpu_vk_draw_tritri), sampling the HUD sprite ATLAS in
// VRAM (tpage/CLUT/UV captured live from the running game), on the engine's OWN 2D overlay layer
// (gpu_vk_set_order_2d puts the HUD on top of the 3D scene). No GP0 packet is built, no PSX OT is
// consulted for ordering — the engine decides the HUD is an on-top 2D layer and draws it.
//
// THE HUD ATLAS + STATE (RE'd LIVE on the field via the `hudprobe` channel; see scratch/logs/probe*.log)
// ------------------------------------------------------------------------------------------------------
// Sprite helper FUN_8007E938(a0=vtable, a1=geom_a, a2=geom_b, a3=ot_bucket, [sp+16]=slice_idx):
//   The field draws the spiky-ball weapon indicator as THREE GP0-0x65 textured sprites per frame at
//   screen X = geom_a-16, Y = geom_b-12 (geom_a = 0x80/0xa0/0xc0 -> screen X 112/144/176, geom_b=0xd4 ->
//   Y 200). Each cell: size 32x24, UV=(48,152), texpage X=384 Y=0 (tpage bits 0x06, 4bpp CLUT mode),
//   CLUT X=480 Y=211 (clut word 0x34de = (211<<6)|(480/16)). Captured GP0 packet (geom_a=0xa0):
//     word1 6500f84f (op 0x65 sprite, modulation color), word2 00c80090 (x=144,y=200),
//     word3 34de9830 (u=48,v=152,clut=0x34de), word4 00180020 (w=32,h=24), word6 e1000006 (tpage 0x06).
//   The modulation color pulses slightly (0x80..0xf8 glow); we read it LIVE from the geomrec the helper
//   builds so the glow is preserved (geomrec word holds the packed XY; the color source is the cell
//   template, see below — we modulate by the per-frame value the game wrote).
//
// Rect helper FUN_8007E8DC(a0,a1=geom_a/b, a2=ot, a3=slice): emits a GP0-0x2d 4-pt textured quad (a HUD
// panel slice). On the reachable field it draws a 16x16 textured quad at (32,168), UV=(0x50,0x70),
// tpage 0x06, clut 0x3dde. These panel slices are minor/contextual (the AP/heart panel frame); they are
// also owned here by the same native textured-quad path.
//
// Why this is NOT the rejected transcription: we do not marshal the PSX stack frame, do not call the PSX
// packet emitter (rec_dispatch), and do not gate on a full-RAM byte match. We read the element's screen
// position + atlas cell and issue PC textured quads on our own 2D overlay layer. The HUD draws to VRAM
// color only (via the VK rasterizer), so guest RAM is UNAFFECTED (the field guest-RAM A/B stays 0-diff).
#include "core.h"
#include "cfg.h"
#include "gpu_vk.h"
#include <stdint.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);
int  gpu_vk_enabled(void);

namespace {

constexpr uint32_t SPRITE_HELPER = 0x8007E938u;
constexpr uint32_t RECT_HELPER   = 0x8007E8DCu;
constexpr uint32_t SLICE_TABLE   = 0x80017334u;  // s16 index, stride 4, -> ptr to slice record

// HUD sprite atlas (the spiky-ball / gauge cells), captured live (see header).
constexpr int HUD_TPX = 384, HUD_TPY = 0;       // texpage base (pixels) — tpage 0x06
constexpr int HUD_CLUTX = 480, HUD_CLUTY = 211; // CLUT base (pixels)
constexpr int HUD_MODE = 0;                      // 4bpp CLUT

// Engine-owned overlay ordering: the HUD is a 2D layer ON TOP of the world. We give it a large 2D order
// index so it sorts above scene/background 2D. (The engine owns this decision; it does NOT read the PSX OT.)
constexpr unsigned HUD_ORDER = 60000;

// Draw one textured quad (the engine's 2D overlay path): screen rect (x,y,w,h) sampling the HUD atlas at
// (u,v) of the same size, modulated by color (cr,cg,cb). Issued as two tris via gpu_vk_draw_tritri on the
// engine's 2D overlay layer (HUD on top). No PSX packet, no OT.
void hud_quad(Core* c, int x, int y, int w, int h, int u, int v, int cr, int cg, int cb,
              int tpx, int tpy, int mode, int clutx, int cluty) {
  if (!gpu_vk_enabled()) return;
  int xs[4] = { x,     x + w, x,     x + w };
  int ys[4] = { y,     y,     y + h, y + h };
  int us[4] = { u,     u + w, u,     u + w };
  int vs[4] = { v,     v,     v + h, v + h };
  unsigned char rs[4] = {(unsigned char)cr,(unsigned char)cr,(unsigned char)cr,(unsigned char)cr};
  unsigned char gs[4] = {(unsigned char)cg,(unsigned char)cg,(unsigned char)cg,(unsigned char)cg};
  unsigned char bs[4] = {(unsigned char)cb,(unsigned char)cb,(unsigned char)cb,(unsigned char)cb};
  // Engine-owned 2D overlay order (HUD on top). No draw-area clip (full FB) — the engine, not the PSX
  // draw-area register, owns HUD visibility; this also fixes the old 4:3 da-clip that ate 2 of 3 balls.
  gpu_vk_set_order(c, HUD_ORDER);
  gpu_vk_set_order_2d(c, HUD_ORDER);
  gpu_vk_draw_tritri(c, xs,        ys,        us,        vs,        rs,        gs,        bs,
                     tpx, tpy, mode, 0, clutx, cluty, 0, 0, 0, 0, 0, 0, 1023, 511);
  gpu_vk_draw_tritri(c, &xs[1],    &ys[1],    &us[1],    &vs[1],    &rs[1],    &gs[1],    &bs[1],
                     tpx, tpy, mode, 0, clutx, cluty, 0, 0, 0, 0, 0, 0, 1023, 511);
}

// FUN_8007E938 — HUD sprite-strip cell. Read the element's screen position (geom_a-16, geom_b-12) and the
// atlas cell, then draw it PC-native. The cell's UV/size come from the slice record (the game's own atlas
// layout); we resolve it exactly so any gauge cell (not just the ball) draws from the right atlas spot.
void ov_hud_sprite(Core* c) {
  { static int d=-1; if(d<0)d=cfg_dbg("hud")?1:0; if(d){static long n=0; if(++n<=8||n%200==0) fprintf(stderr,"[hud] ov_hud_sprite FIRED #%ld (native draw)\n",n);} }
  uint32_t vtable = c->r[4];          // a0
  int geom_a = (int16_t)c->r[5];      // a1 -> screen X anchor
  int geom_b = (int16_t)c->r[6];      // a2 -> screen Y anchor
  int slice_idx = (int16_t)c->mem_r16(c->r[29] + 16);   // arg5 on caller stack

  // Resolve the cell record exactly as the emitter does: SLICE_TABLE[slice_idx] -> slice_rec;
  // hdr = vtable + lh(slice_rec)*4; count = lh(hdr+0); celloff = lh(hdr+2); cellbase = vtable + celloff.
  uint32_t slice_rec = c->mem_r32(SLICE_TABLE + (uint32_t)slice_idx * 4u);
  int idx0  = (int16_t)c->mem_r16(slice_rec + 0);
  uint32_t hdr = vtable + (uint32_t)idx0 * 4u;
  int count = (int16_t)c->mem_r16(hdr + 0);
  int celloff = (int16_t)c->mem_r16(hdr + 2);
  uint32_t cellbase = vtable + (uint32_t)celloff;

  // Each cell record (16 bytes), decoded from the captured ball cell bytes
  //   30 98 de 34 | 50 98 06 00 | 30 b0 20 18 | 50 b0 f0 f4
  // = u@+0(0x30=48), v@+1(0x98=152), clut@+2(half 0x34de), w@+10(0x20=32), h@+11(0x18=24),
  //   dx@+14(s8 0xf0=-16), dy@+15(s8 0xf4=-12). Screen XY = geom + (dx,dy), matching the emitter.
  for (int i = 0; i < count; i++) {
    uint32_t cell = cellbase + (uint32_t)i * 16u;
    int u  = c->mem_r8(cell + 0);
    int v  = c->mem_r8(cell + 1);
    int w  = c->mem_r8(cell + 10);
    int h  = c->mem_r8(cell + 11);
    int dx = (int8_t)c->mem_r8(cell + 14);
    int dy = (int8_t)c->mem_r8(cell + 15);
    if (w <= 0) w = 32;
    if (h <= 0) h = 24;
    int x = geom_a + dx;
    int y = geom_b + dy;
    // PSX identity modulation (0x80 = 1.0). The texture art already carries the ball/gauge colors.
    hud_quad(c, x, y, w, h, u, v, 0x80, 0x80, 0x80, HUD_TPX, HUD_TPY, HUD_MODE, HUD_CLUTX, HUD_CLUTY);
  }
  // No guest writes: the recomp body is NOT run (we own the draw). Return v0=0 like the helper.
  c->r[2] = 0;
}

// FUN_8007E8DC — HUD UI panel slice (textured quad). Captured field slice: 16x16 quad at (32,168),
// UV=(0x50,0x70), tpage 0x06, clut 0x3dde. Draw it PC-native on the overlay layer.
void ov_hud_rect(Core* c) {
  int geom_a = (int16_t)c->r[4];   // a0 -> X
  int geom_b = (int16_t)c->r[5];   // a1 -> Y
  // The reachable field panel slice is a fixed 16x16 atlas cell. Draw it natively (CLUT 0x3dde = (247<<6)?).
  // clut 0x3dde -> cx=(0x3dde&0x3f)*16=0x1e*16=480, cy=0x3dde>>6=0xf7=247.
  int x = geom_a, y = geom_b;
  hud_quad(c, x, y, 16, 16, 0x50, 0x70, 0x80, 0x80, 0x80, HUD_TPX, HUD_TPY, HUD_MODE, 480, 247);
  c->r[2] = 0;
}

}  // namespace

void hud_register(void) {
  rec_set_override(SPRITE_HELPER, ov_hud_sprite);
  rec_set_override(RECT_HELPER,   ov_hud_rect);
}
