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
#include "render_queue.h"
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
constexpr unsigned HUD_ORDER = 60000;            // overlay order for the `texttest` placement diagnostic only
                                                 // (the real HUD/glyph draw goes through the render queue via hud_quad)

// Engine-owned overlay ordering: the HUD is a 2D layer ON TOP of the world — the queue's RQ_HUD layer
// sorts it above the world/2D. (The engine owns this decision; it does NOT read the PSX OT.)

// Draw one textured quad (the engine's 2D overlay path): screen rect (x,y,w,h) sampling the HUD atlas at
// (u,v) of the same size, modulated by color (cr,cg,cb). Enqueued as an RqItem on the engine's HUD layer
// (RQ_HUD, on top) via rq_push_2d_quad — so the HUD is part of THE FRAME (the render queue) and is
// re-emitted on BOTH 60fps present passes (no direct gpu_vk_draw_tritri that lands on only one pass and
// strobes). No PSX packet, no OT; the queue owns 2D order (HUD layer above the world).
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
  // No draw-area clip (full FB) — the engine, not the PSX draw-area register, owns HUD visibility (this is
  // what fixed the old 4:3 da-clip that ate 2 of 3 balls). RQ_HUD sorts above the world; 2D-FG depth band.
  rq_push_2d_quad(c, RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, rs, gs, bs,
                  tpx, tpy, mode, /*raw=*/0, clutx, cluty, 0, 0, 0, 0, 0, 0, 1023, 511);
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

// =====================================================================================================
// PC-NATIVE TEXT GLYPHS — own the in-game string drawer FUN_80078CA8 (the engine's UI font emitter).
// =====================================================================================================
// RE (tools/disas.py 0x80078ca8; UV math 0x80078e70; live-confirmed on the field intro narration):
//   FUN_80078CA8(a0, a1, a2, a3=string, [sp+16]=fontslot)
//     a0 = PACKED PEN: low16 = pen X, high16 = pen Y (the prim's GP0 xy word). Live: 0x00df001c => x=28,y=223.
//     a1 = PACKED CELL: low16 = cell WIDTH (advance), high16 = cell HEIGHT. Live: 0x00100008 => w=8,h=16.
//     a2 = FONT BANK index (selects the CLUT): if a2<16  clut = ((a2+496)<<6)|0x3f
//                                              else      clut = ((a2+480)<<6)|0x3e   (live a2=0 => 0x7c3f).
//     a3 = NUL-terminated string pointer.
//   Per char the body builds an op-0x65 textured sprite (font atlas) and links it into the OT pool; the
//   atlas CELL is computed from the char code:  idx = ch + fontbase(*0x1f800180, s16);
//     U = (idx & 31) * cellW ;  row = idx>>5 ; V = row*cellH (+8 when cellH==16) — verified: 'T'(0x54)+32=116
//     => col20*8=160=U, row3*16+8=56=V, matching the captured prim uv=(160,56).
//   The prim COLOR (modulation, e.g. the narration fade-in 0xf0..0xc0 ramp) is the GP0 sprite cmd word's
//   R/G/B bytes, held in the scratchpad template at 0x1f800004(R)/+5(G)/+6(B) (the prior "set text colour"
//   call wrote them; they persist between glyph calls). Control codes 0x01..0x04 (format/colour escapes)
//   and 0x20 (space)/0x0a (newline) are handled like the body.
// We REBUILD this PC-native (CLAUDE.md engine UI rule): instead of emitting a PSX op-0x65 packet into the
// guest OT, we read the SAME state and draw each glyph as a PC textured quad from the font atlas on the
// engine's OWN 2D overlay layer (gpu_vk_draw_tritri + gpu_vk_set_order_2d), exactly like ov_hud_sprite.
// No guest writes (the recomp body is NOT run) -> the content interface / guest RAM is untouched.
//
// Font atlas tpage: tp=(960,256), 4bpp (docs/engine_re.md "font/UI texpage tp=(960,256)"); CLUT from a2.
constexpr int FONT_TPX = 960, FONT_TPY = 256, FONT_MODE = 0;   // 4bpp font atlas

// ---- TEMPORARY RE PROBES (bannerprobe channel): log box/9slice emitter state, super-call ----
constexpr uint32_t GLYPH_STR  = 0x80078CA8u;  // per-glyph string drawer (loops a0..a3 over a string)
constexpr uint32_t UI_RECT    = 0x8007E1B8u;  // universal UI rect emitter (box / panel slice)
constexpr uint32_t NINE_SLICE = 0x8007EAE4u;  // 9-slice panel (box frame)

// PC-native glyph string drawer. Mirrors FUN_80078CA8's char loop + atlas-cell UV math, but draws each
// glyph as a textured quad on the engine 2D overlay instead of building a PSX op-0x65 OT packet.
void ov_glyph_string(Core* c) {
  { static int d=-1; if(d<0)d=cfg_dbg("hud")?1:0; if(d){static long n=0; if(++n<=4||n%400==0) fprintf(stderr,"[hud] ov_glyph_string FIRED #%ld (native text)\n",n);} }
  uint32_t a0 = c->r[4], a1 = c->r[5];
  int32_t  a2 = (int32_t)c->r[6];
  uint32_t s  = c->r[7];                       // string ptr

  int penx = (int16_t)(a0 & 0xffff);
  int peny = (int16_t)(a0 >> 16);
  int cellW = (int16_t)(a1 & 0xffff);
  int cellH = (int16_t)(a1 >> 16);
  if (cellW <= 0) cellW = 8;
  if (cellH <= 0) cellH = 16;

  // CLUT (bank a2) — exactly as the emitter computes it.
  uint32_t clutw = (a2 < 16) ? (uint32_t)(((a2 + 496) << 6) | 0x3f)
                             : (uint32_t)(((a2 + 480) << 6) | 0x3e);
  int clutx = (int)((clutw & 0x3f) * 16);
  int cluty = (int)(clutw >> 6);

  int fontbase = (int16_t)c->mem_r16(0x1f800180u);   // s16 glyph-index base

  // Escape/special tokens (bytes 0x01..0x04) are NOT printable glyphs: the recomp body parses them as
  // MULTI-BYTE tokens via FUN_80078988 (token table @0x800a55e0 — special symbols / button-prompt icons).
  // We do not yet own that token atlas, and parsing them as single bytes would DESYNC the string scan.
  // So if any such token is present, fall back to the faithful recomp body for the WHOLE string (text
  // stays correct everywhere); we own the common plain-text case PC-native. (Plain narration/menu/help
  // strings have none — verified.)
  for (uint32_t p = s; ; p++) { uint8_t ch = c->mem_r8(p); if (ch == 0) break; if (ch >= 0x01 && ch <= 0x04) { rec_super_call(c, 0x80078CA8u); return; } }

  if (cfg_dbg("bannerprobe")) {
    char buf[64]; int n=0; for(;n<63;n++){uint8_t ch=c->mem_r8(s+(uint32_t)n); if(!ch)break; buf[n]=(ch>=0x20&&ch<0x7f)?(char)ch:'.';} buf[n]=0;
    static long cnt=0; if(++cnt<=20) fprintf(stderr,"[bp] NATIVE glyph pen=(%d,%d) cell=%dx%d bank=%d clut=(%d,%d) base=%d \"%s\"\n",penx,peny,cellW,cellH,a2,clutx,cluty,fontbase,buf);
  }

  int x = penx, y = peny;
  for (;;) {
    uint8_t ch = c->mem_r8(s); s++;
    if (ch == 0) break;
    if (ch == 0x20) { x += 8; continue; }              // space: advance pen (matches body's +8)
    if (ch == 0x0a) { x = penx; continue; }            // newline: reset X (Y handled by caller per line)
    // (0x01..0x04 tokens were handled up-front by the recomp fallback, so they never reach here.)
    // Per-glyph colour from the scratchpad template (set by the prior "set text colour" call).
    int cr = c->mem_r8(0x1f800004u);
    int cg = c->mem_r8(0x1f800005u);
    int cb = c->mem_r8(0x1f800006u);
    if (cr==0 && cg==0 && cb==0) { cr=cg=cb=0x80; }     // template not yet set -> identity (no modulation)

    int idx = (int)ch + fontbase;
    int u = (idx & 31) * cellW;
    int row = idx >> 5;
    int v = row * cellH + (cellH == 16 ? 8 : 0);

    if (cfg_dbg("texttest")) {   // DIAG: draw solid magenta cells to confirm placement (no texture sample)
      int xs[4]={x,x+cellW,x,x+cellW}, ys[4]={y,y,y+cellH,y+cellH};
      int us[4]={0,0,0,0}, vs[4]={0,0,0,0};
      unsigned char R[4]={255,255,255,255},G[4]={0,0,0,0},B[4]={255,255,255,255};
      gpu_vk_set_order(c, HUD_ORDER); gpu_vk_set_order_2d(c, HUD_ORDER);
      gpu_vk_draw_tritri(c, xs,ys,us,vs,R,G,B, FONT_TPX,FONT_TPY,FONT_MODE,1,clutx,cluty,0,0,0,0,0,0,1023,511);
      gpu_vk_draw_tritri(c, &xs[1],&ys[1],&us[1],&vs[1],&R[1],&G[1],&B[1], FONT_TPX,FONT_TPY,FONT_MODE,1,clutx,cluty,0,0,0,0,0,0,1023,511);
      x += 8; continue;
    }
    hud_quad(c, x, y, cellW, cellH, u, v, cr, cg, cb, FONT_TPX, FONT_TPY, FONT_MODE, clutx, cluty);
    x += 8;                                             // pen advance (body uses fixed +8 per glyph)
  }
  c->r[2] = 0;   // body returns the pool head ptr in v0; the caller of this drawer ignores it (chained text).
}

void probe_rect(Core* c) {
  if (cfg_dbg("bannerprobe")) {
    uint32_t a0=c->r[4],a1=c->r[5],a2=c->r[6],a3=c->r[7];
    int w=(int16_t)c->mem_r16(a0+4), h=(int16_t)c->mem_r16(a0+6);
    uint16_t d2=c->mem_r16(a3+2);
    static long cnt=0; if(++cnt<=80) fprintf(stderr,"[bp] rect geom=%08x w=%d h=%d idx@a1=%08x tbl=%08x desc=%08x op=%02x ot=%02x d2=%04x\n",
        a0,w,h,a1,a2,a3,c->mem_r8(a3),c->mem_r8(a3+1),d2);
  }
  rec_super_call(c, UI_RECT);
}
void probe_nine(Core* c) {
  if (cfg_dbg("bannerprobe")) { static long cnt=0; if(++cnt<=20) fprintf(stderr,"[bp] 9slice a0=%08x a1=%08x a2=%08x a3=%08x\n",c->r[4],c->r[5],c->r[6],c->r[7]); }
  rec_super_call(c, NINE_SLICE);
}

}  // namespace

void hud_register(void) {
  rec_set_override(SPRITE_HELPER, ov_hud_sprite);
  rec_set_override(RECT_HELPER,   ov_hud_rect);
  // PC-native text glyph drawer (own FUN_80078CA8; draws font quads on the engine 2D overlay) — THE behavior.
  rec_set_override(GLYPH_STR,  ov_glyph_string);
  // Box / 9-slice frame: not yet owned. Registered as pure super-call (behavior-identical to the recomp
  // body) plus the `bannerprobe` diagnostic log, so the next session can RE them on the live banner scene.
  rec_set_override(UI_RECT,    probe_rect);
  rec_set_override(NINE_SLICE, probe_nine);
}
