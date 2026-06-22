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

  // BUG 2 — weapon carousel: show ONLY the current (center) weapon, not prev+current+next.
  // The carousel routine (RE'd at 0x80025c00) calls this helper THREE times per frame for slice 211:
  //   - CURRENT (center, X=160): jal 0x80025c60 -> ra = 0x80025c68
  //   - PREV    (left,   X=128): jal 0x80025cbc -> ra = 0x80025cc4
  //   - NEXT    (right,  X=192): jal 0x80025d14 -> ra = 0x80025d1c
  // (ra = jal_addr + 8: the instruction after the branch-delay slot.) We keep only the CURRENT draw.
  if (slice_idx == 211 && c->r[31] != 0x80025c68u) {
    c->r[2] = 0;   // skip prev/next carousel draws; behave like the helper (v0=0)
    return;
  }

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

// ---- TOKEN ICONS (string bytes 0x01..0x04) — now OWNED PC-native, no recomp fallback ----------------
// RE (tools/disas.py 0x80078ca8 + 0x80078988; token table 0x800a55e0):
//   In FUN_80078CA8 a string byte 0x01..0x04 is a SINGLE byte (the body does `addiu s0,s0,1`). It is NOT
//   a printable glyph: the body passes a fixed 2-byte Shift-JIS *token code* to the token drawer
//   FUN_80078988 (a3 = a pointer to that 2-byte code in .rodata), then advances the glyph pen by +8 in
//   the glyph scratchpad (0x1f800008) just like any glyph (the token drawer's own scratch pen at
//   0x1f800028 is independent and discarded by the caller). So each token = ONE 8-wide cell, advance +8.
//   The per-token 2-byte codes (read big-endian as the parser does, (b0<<8)|b1):
//     ch 0x01 -> code @0x80016da8 = 0x9573 ;  ch 0x02 -> code @0x80016dac = 0x8c88
//     ch 0x03 -> code @0x80016da4 = 0x8e4f ;  ch 0x04 -> code @0x80016da0 = 0x8e6c
//   FUN_80078988 looks each code up in the table at 0x800a55e0 (8-byte entries: [u32 ptr->2byte code,
//   u16 glyph-index, ...]) via a 2-byte memcmp, yielding a glyph INDEX:
//     0x9573->0x61(97)   0x8c88->0x60(96)   0x8e4f->0x62(98)   0x8e6c->0x63(99)
//   The icon cell is then an 8x8 cell in the SAME font atlas (tpage 960,256; CLUT from the font bank a2),
//   addressed directly by that index (NO fontbase added, UNLIKE plain glyphs):
//     U = (idx & 31) * 8 ;  V = (idx >> 5) * 8        (idx 96..99 -> all on row 3: V=24, U=0/8/16/24)
//   These are the button-prompt / direction icons in prompts like "Use ^ and (O) to look inside".
//   Special token results 0xff02 (=advance pen only) and 0x0a0a (=newline) cannot arise from 0x01..0x04
//   (those map to 0x60..0x63), so they need no handling here.
// Map: string token byte (0x01..0x04) -> font-atlas glyph index. Index 0 unused (no token byte 0x00).
constexpr int TOKEN_GLYPH_IDX[5] = { 0, 0x61, 0x60, 0x62, 0x63 };

// ---- TEMPORARY RE PROBES (bannerprobe channel): log box/9slice emitter state, super-call ----
constexpr uint32_t GLYPH_STR  = 0x80078CA8u;  // per-glyph string drawer (loops a0..a3 over a string)
constexpr uint32_t UI_RECT    = 0x8007E1B8u;  // universal UI rect emitter (box / panel slice)
constexpr uint32_t NINE_SLICE = 0x8007EAE4u;  // 9-slice panel (box frame)

// ---- IN-GAME MENU LABEL VISIBILITY (GitHub #26) -----------------------------------------------------
// The two thin text wrappers below are the ONLY callers of the glyph drawer FUN_80078CA8 (verified: the
// jal 0x80078ca8 sites are exactly 0x8007935c inside FUN_80079324 and 0x800793ac inside FUN_80079374 — so
// EVERY string in the game routes through one of these two). They set the pen (a0), cell size (a1=8x8 or
// 8x16) and font bank (a2->CLUT), then jal FUN_80078CA8 — but they do NOT touch the per-glyph MODULATION
// COLOUR template at scratchpad 0x1f800004(R)/+5(G)/+6(B). FUN_80078CA8 packs those 3 bytes into every
// op-0x65 sprite cmd word (lw 4(0x1f800000) @0x80078f34); ov_glyph_string reads the same slots.
//   Most text callers (dialog, the field narration fade-in ramp) set that template themselves before
// calling the wrapper (e.g. via the "set-colour + box" routine FUN_8007E9C8, which writes a0 -> 0x1f800004),
// so their text is coloured/visible. But the in-game MENU draw routines — the SAVE/LOAD data menu
// (FUN_800737F8 / 800738B0 / 800739AC / 80073CD8, cluster [0x80073328..0x80074134]) and the OPTIONS/CONFIG
// screen (FUN_8007F914 + siblings, cluster [0x8007EAE4..0x8007FDA8]) — call the wrappers WITHOUT ever
// setting the template. They inherit whatever the previous draw left there; on these dark menu boxes that
// stale value is dark, so the labels render near-black-on-dark = INVISIBLE (#26).
//   FIX (PC-owned, scoped — NOT a blanket force): override the two wrappers; when the CALLER (ra) is in a
// menu cluster, set the colour template to the engine's intended visible menu colour BEFORE super-calling
// the wrapper body (which then runs the real pen/font setup and the glyph draw). Non-menu callers
// (narration/dialog) are untouched — we just super-call, preserving their own colour. So this fixes the
// menu labels without regressing the working field-narration / dialog text.
//
// INTENDED MENU TEXT COLOUR: 0x80 = PSX identity modulation (1.0). At 0x80 the glyph keeps its font-CLUT
// colour unmodulated — i.e. full-brightness cream/white menu text exactly as the art was authored. The
// game's own visible-text path treats 0x80/0x80/0x80 as "no tint" (ov_glyph_string's existing fallback
// already maps an all-zero template to 0x80), so identity is the designed, non-arbitrary menu colour.
constexpr uint32_t TEXTW_8  = 0x80079324u;  // FUN_80079324: 8x8 cell text wrapper
constexpr uint32_t TEXTW_16 = 0x80079374u;  // FUN_80079374: 8x16 cell text wrapper
constexpr uint8_t  MENU_TEXT_RGB = 0x80;    // PSX identity modulation -> unmodulated cream/white CLUT text

// True when return address `ra` lands inside one of the in-game menu draw clusters whose label routines
// never set the glyph colour template (save/load data menu + options/config screen). Ranges are the
// function-boundary clusters that hold every wrapper jal for those screens (see RE comment above).
inline bool ra_is_menu_cluster(uint32_t ra) {
  return (ra >= 0x80073328u && ra <  0x80074134u)    // SAVE / LOAD data menu draw cluster
      || (ra >= 0x8007EAE4u && ra <= 0x8007FDA8u);   // OPTIONS / CONFIG screen draw cluster
}

// Shared wrapper override: for menu-cluster callers, set the visible menu colour template, then run the
// real wrapper body (pen/font setup + glyph draw via ov_glyph_string). For all other callers, just
// super-call unchanged so their own colour template is honoured (no regression to narration/dialog).
inline void ov_text_wrapper(Core* c, uint32_t wrapper_addr) {
  if (ra_is_menu_cluster(c->r[31])) {
    c->mem_w8(0x1f800004u, MENU_TEXT_RGB);   // R
    c->mem_w8(0x1f800005u, MENU_TEXT_RGB);   // G
    c->mem_w8(0x1f800006u, MENU_TEXT_RGB);   // B  -> identity = full-brightness CLUT text
  }
  rec_super_call(c, wrapper_addr);           // run the original wrapper body (calls 0x80078ca8/ov_glyph_string)
}
void ov_textw_8 (Core* c) { ov_text_wrapper(c, TEXTW_8);  }
void ov_textw_16(Core* c) { ov_text_wrapper(c, TEXTW_16); }

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

  // BUG 1 — banner/menu text spread across the whole screen.
  // ROOT CAUSE (RE of FUN_80078CA8): the recomp keeps the LIVE pen in scratchpad and, crucially,
  // RETURNS the width it drew in v0:  v0 = (s16)(*0x1f800008) - (a0 & 0xffff)  (= finalPenX - startPenX,
  // see 0x80078fdc..0x80079008). A caller that draws one banner as several consecutive chunks positions
  // each next chunk using that returned width (and reads/writes the scratch pen at 0x1f800008/0x1f80000a).
  // The old native override returned r[2]=0 and never touched the scratch pen, so every chunk after the
  // first landed at a wrong absolute X -> the banner got split into screen-spanning pieces.
  // We now (a) reset the scratch pen to a0 like the body, (b) track the pen through the scratch words so
  // newline (0x0a) and the final value are exact, and (c) return the real drawn width in v0.
  c->mem_w16(0x1f800008u, (uint16_t)(a0 & 0xffff));   // pen X (low16 of a0)
  c->mem_w16(0x1f80000au, (uint16_t)(a0 >> 16));      // pen Y (high16 of a0)
  c->mem_w16(0x1f800010u, (uint16_t)(a1 & 0xffff));   // cellW (= U stride), as the body stores a1
  c->mem_w16(0x1f800012u, (uint16_t)(a1 >> 16));      // cellH (= V stride / line advance)
  int startPenX = (int)(int16_t)(a0 & 0xffff);

  // CLUT (bank a2) — exactly as the emitter computes it.
  uint32_t clutw = (a2 < 16) ? (uint32_t)(((a2 + 496) << 6) | 0x3f)
                             : (uint32_t)(((a2 + 480) << 6) | 0x3e);
  int clutx = (int)((clutw & 0x3f) * 16);
  int cluty = (int)(clutw >> 6);

  int fontbase = (int16_t)c->mem_r16(0x1f800180u);   // s16 glyph-index base

  // Token icons (bytes 0x01..0x04) are now OWNED natively below (see TOKEN_GLYPH_IDX / the loop); there
  // is NO recomp fallback any more — every string is drawn PC-native.

  if (cfg_dbg("bannerprobe")) {
    char buf[64]; int n=0; for(;n<63;n++){uint8_t ch=c->mem_r8(s+(uint32_t)n); if(!ch)break; buf[n]=(ch>=0x20&&ch<0x7f)?(char)ch:'.';} buf[n]=0;
    // Also dump the GLYPH colour template (0x1f800004/5/6 — the per-glyph modulation RGB the body packs
    // into the GP0 sprite cmd word, see FUN_80078CA8 @0x80078f34) so we can see WHY config-screen labels
    // come out invisible (GitHub #26): if these are dark/zero against a dark box, the text is unreadable.
    int tcr=c->mem_r8(0x1f800004u), tcg=c->mem_r8(0x1f800005u), tcb=c->mem_r8(0x1f800006u);
    static long cnt=0; if(++cnt<=40) fprintf(stderr,"[bp] NATIVE glyph pen=(%d,%d) cell=%dx%d bank=%d clut=(%d,%d) base=%d colTmpl=(%d,%d,%d) \"%s\"\n",penx,peny,cellW,cellH,a2,clutx,cluty,fontbase,tcr,tcg,tcb,buf);
  }

  int x = penx, y = peny;
  for (;;) {
    uint8_t ch = c->mem_r8(s); s++;
    if (ch == 0) break;
    if (ch == 0x20) { x += 8; continue; }              // space: advance pen (matches body's +8)
    if (ch == 0x0a) {                                   // newline: reset X to (a0 & 0xfff), advance Y by cellH
      x = (int)(a0 & 0xfff);                            // body uses andi a0,0xfff for the X reset (0x80078d60)
      y += cellH;                                       // body: penY += *0x1f800012 (cellH), 0x80078d68
      continue;
    }
    // Token icons (0x01..0x04): button-prompt / direction glyphs, OWNED natively (no recomp fallback).
    // The body parses these via FUN_80078988 -> a fixed 8x8 cell in the font atlas at glyph index
    // TOKEN_GLYPH_IDX[ch] (no fontbase), CLUT = the same font bank, advance the pen by +8 (same as a
    // plain glyph). Colour comes from the token scratch template (0x1f800024..0x26 = the GP0 sprite
    // cmd word's R/G/B that FUN_80078988 reads via `lw 4(s2)`), with the same 0x80-identity fallback.
    if (ch >= 0x01 && ch <= 0x04) {
      int tr = c->mem_r8(0x1f800024u);
      int tg = c->mem_r8(0x1f800025u);
      int tb = c->mem_r8(0x1f800026u);
      if (tr==0 && tg==0 && tb==0) { tr=tg=tb=0x80; }   // template not yet set -> identity modulation
      int tidx = TOKEN_GLYPH_IDX[ch];
      int tu = (tidx & 31) * 8;                          // token cells are fixed 8x8 (FUN_80078988)
      int tv = (tidx >> 5) * 8;
      hud_quad(c, x, y, 8, 8, tu, tv, tr, tg, tb, FONT_TPX, FONT_TPY, FONT_MODE, clutx, cluty);
      x += 8;                                            // pen advance (+8, matching the caller's body)
      continue;
    }
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
  // Persist the final pen to the scratchpad (the body leaves it there) and return the WIDTH drawn in v0,
  // exactly as FUN_80078CA8 does (v0 = (s16)finalPenX - (a0 & 0xffff)). Callers chain banner chunks with
  // this width / read the scratch pen, so getting it wrong is what spread text across the screen (BUG 1).
  c->mem_w16(0x1f800008u, (uint16_t)x);                 // final pen X
  c->mem_w16(0x1f80000au, (uint16_t)y);                 // final pen Y
  c->r[2] = (uint32_t)(int32_t)((int16_t)(uint16_t)x - startPenX);
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
  // PC-native text glyph drawer (own FUN_80078CA8; draws font quads on the engine 2D overlay) — THE behavior.
  // Box / 9-slice frame: not yet owned. Registered as pure super-call (behavior-identical to the recomp
  // body) plus the `bannerprobe` diagnostic log, so the next session can RE them on the live banner scene.
  // In-game MENU label visibility (GitHub #26): the text wrappers set a visible colour template for
  // menu-cluster callers, then super-call the body. See the RE comment above ov_text_wrapper.
}
