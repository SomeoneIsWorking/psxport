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
// with the PC renderer's 2D textured-quad path (gpu_gpu_draw_tritri), sampling the HUD sprite ATLAS in
// VRAM (tpage/CLUT/UV captured live from the running game), on the engine's OWN 2D overlay layer
// (gpu_gpu_set_order_2d puts the HUD on top of the 3D scene). No GP0 packet is built, no PSX OT is
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
#include "gpu_gpu.h"
#include "render_queue.h"
#include "game.h"    // c->game->rq.push2dQuad
#include <stdint.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);
int  gpu_gpu_enabled(void);

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
// re-emitted on BOTH 60fps present passes (no direct gpu_gpu_draw_tritri that lands on only one pass and
// strobes). No PSX packet, no OT; the queue owns 2D order (HUD layer above the world).
void hud_quad(Core* c, int x, int y, int w, int h, int u, int v, int cr, int cg, int cb,
              int tpx, int tpy, int mode, int clutx, int cluty) {
  if (!gpu_gpu_enabled()) return;
  int xs[4] = { x,     x + w, x,     x + w };
  int ys[4] = { y,     y,     y + h, y + h };
  int us[4] = { u,     u + w, u,     u + w };
  int vs[4] = { v,     v,     v + h, v + h };
  unsigned char rs[4] = {(unsigned char)cr,(unsigned char)cr,(unsigned char)cr,(unsigned char)cr};
  unsigned char gs[4] = {(unsigned char)cg,(unsigned char)cg,(unsigned char)cg,(unsigned char)cg};
  unsigned char bs[4] = {(unsigned char)cb,(unsigned char)cb,(unsigned char)cb,(unsigned char)cb};
  // No draw-area clip (full FB) — the engine, not the PSX draw-area register, owns HUD visibility (this is
  // what fixed the old 4:3 da-clip that ate 2 of 3 balls). RQ_HUD sorts above the world; 2D-FG depth band.
  c->game->rq.push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, rs, gs, bs,
                         tpx, tpy, mode, /*raw=*/0, clutx, cluty, 0, 0, 0, 0, 0, 0, 1023, 511);
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
// engine's OWN 2D overlay layer (gpu_gpu_draw_tritri + gpu_gpu_set_order_2d), exactly like ov_hud_sprite.
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

void probe_rect(Core* c) {
  if (cfg_dbg("bannerprobe")) {
    uint32_t a0=c->r[4],a1=c->r[5],a2=c->r[6],a3=c->r[7];
    int w=c->mem_r16s(a0+4), h=c->mem_r16s(a0+6);
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

