// class Panel — implementation. See panel.h for the architecture note.
#include "panel.h"
#include "core.h"
#include "game.h"
#include "render.h"          // Render::mode.psxRender() gate
#include "render_queue.h"    // RenderQueue::push2dQuad / emitOrQueue + RQ_HUD / RQ_OM_2D_FG
#include "cfg.h"              // cfg_logf panelq probe

// Both builders share the one skin-texture atlas texpage (VRAM 960,256) — docs/native-render-2d-panel.md
// "Skin-texture atlas at VRAM (960,256)".
namespace {
constexpr uint16_t kPanelTpage = 0x5Fu;

struct TpageDecode { int x, y, mode, blend; };
// Same bit layout as GpuState::set_texpage (runtime/recomp/gpu_native.cpp) — tp_x=(tp&0xF)*64,
// tp_y=((tp>>4)&1)*256, blend=(tp>>5)&3, mode=(tp>>7)&3.
TpageDecode decodeTpage(uint16_t tp) {
  TpageDecode d;
  d.x = (tp & 0xFu) * 64;
  d.y = ((tp >> 4) & 1u) * 256;
  d.blend = (tp >> 5) & 3u;
  d.mode = (tp >> 7) & 3u;
  return d;
}

// Shared attr bit-decode (docs/native-render-2d-panel.md "Shared attr bit-decode") — used
// identically by the FT4 fill (Spec 2) and the corner sprites (Spec 1).
struct AttrDecode { int semi; int raw; unsigned char rgb; int clut_x; int clut_y; };
AttrDecode decodeAttr(uint16_t attr) {
  AttrDecode a;
  a.semi = (attr & 0x80u) ? 1 : 0;
  a.raw  = (attr & 0x20u) ? 0 : 1;          // 0x20 set -> RGB-modulated, clear -> raw texel
  a.rgb  = a.raw ? 0x80u : 0x40u;
  const int clut = (((attr & 0x1Fu) + 0x1F0u) << 6) | ((attr & 0x40u) ? 0x3Fu : 0x3Eu);
  a.clut_x = (clut & 0x3F) * 16;
  a.clut_y = (clut >> 6) & 0x1FF;
  return a;
}

// One native quad push, shared by the fill and each corner sprite — opaque via push2dQuad, semi
// via emitOrQueue (same split as every other native 2D producer in this codebase, e.g.
// Font::glyphQueuePush / Render::dialogTextNative).
// Layer: RQ_OVERLAY, one band BELOW the glyph text's RQ_HUD — the queue sorts (layer, seq), so the
// box (fill/border/corners) always composites UNDER its text regardless of guest emit order. With
// both on RQ_HUD the fill drew over the glyphs whenever the panel emitter ran after the text
// emitter (USER screenshots 2026-07-16, bug #64: dimmed text behind the fill / fully empty boxes).
void pushQuad(Core* c, const int* xs, const int* ys, const int* us, const int* vs,
              const AttrDecode& pa, const TpageDecode& tp) {
  const unsigned char cc[4] = { pa.rgb, pa.rgb, pa.rgb, pa.rgb };
  if (!pa.semi) {
    c->game->activeRq().push2dQuad(RQ_OVERLAY, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                   tp.x, tp.y, tp.mode, pa.raw, pa.clut_x, pa.clut_y,
                                   0, 0, 0, 0, 0, 0, 1023, 511);
  } else {
    c->game->activeRq().emitOrQueue(c, /*capture=*/1, RQ_OVERLAY, RQ_OM_2D_FG, /*nv=*/4, /*semi=*/1, pa.raw,
                                    xs, ys, nullptr, nullptr, us, vs, cc, cc, cc, /*depth=*/nullptr,
                                    tp.mode, tp.x, tp.y, pa.clut_x, pa.clut_y,
                                    0, 0, 0, 0, 0, 0, 1023, 511, tp.blend);
  }
}
} // namespace

// pushFill — Spec 2. Verts v0(x,y) v1(x+w,y) v2(x,y+h) v3(x+w,y+h); UV per uvIndex (v0=uL,vT /
// v1=uR,vT / v2=uL,vB / v3=uR,vB), table straight from the doc.
void Panel::pushFill(Core* c, uint32_t rectPtr, int32_t uvIndex, uint16_t attr, int32_t otBucket) {
  (void)otBucket;   // OT bucket only matters for the guest packet chain (gen already ran it)
  static const struct { int uL, uR, vT, vB; } kUv[5] = {
    { 192, 200, 136, 144 },   // 0 top
    { 240, 248, 136, 144 },   // 1 bottom
    { 208, 216, 137, 143 },   // 2 left
    { 224, 232, 137, 143 },   // 3 right
    { 216, 223, 136, 143 },   // 4 center
  };
  if (uvIndex < 0 || uvIndex > 4) return;
  const auto& uv = kUv[uvIndex];

  const int rx = c->mem_r16s(rectPtr + 0u);
  const int ry = c->mem_r16s(rectPtr + 2u);
  const int rw = c->mem_r16s(rectPtr + 4u);
  const int rh = c->mem_r16s(rectPtr + 6u);
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;

  const AttrDecode pa = decodeAttr(attr);
  const TpageDecode tp = decodeTpage(kPanelTpage);

  int xs[4] = { rx + ox, rx + rw + ox, rx + ox,      rx + rw + ox };
  int ys[4] = { ry + oy, ry + oy,      ry + rh + oy, ry + rh + oy };
  int us[4] = { uv.uL, uv.uR, uv.uL, uv.uR };
  int vs[4] = { uv.vT, uv.vT, uv.vB, uv.vB };

  { static long np = 0; if ((np++ & 127) == 0)
      cfg_logf("panelq", "fill #%ld uv=%d rect=(%d,%d %dx%d) attr=%04X semi=%d raw=%d",
               np, uvIndex, rx, ry, rw, rh, attr, pa.semi, pa.raw); }

  pushQuad(c, xs, ys, us, vs, pa, tp);
}

// pushCorners — Spec 1. attr = style&0x40 ? style+0x0D : (style+6)|(shadow?0x80:0); 4 SPRT_8 8x8
// corners: TL(x-8,y-8,u=184) TR(x+w,y-8,u=200) BL(x-8,y+h,u=232) BR(x+w,y+h,u=248), all v=136.
void Panel::pushCorners(Core* c, uint32_t rectPtr, uint16_t style, uint32_t shadow, int32_t otBucket) {
  (void)otBucket;
  const uint16_t attr = (style & 0x40u)
      ? (uint16_t)(style + 0x0Du)
      : (uint16_t)((style + 6u) | (shadow ? 0x80u : 0u));

  const int rx = c->mem_r16s(rectPtr + 0u);
  const int ry = c->mem_r16s(rectPtr + 2u);
  const int rw = c->mem_r16s(rectPtr + 4u);
  const int rh = c->mem_r16s(rectPtr + 6u);
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;

  const AttrDecode pa = decodeAttr(attr);
  const TpageDecode tp = decodeTpage(kPanelTpage);

  struct Corner { int x, y, u; };
  const Corner corners[4] = {
    { rx - 8,  ry - 8,  184 },   // TL
    { rx + rw, ry - 8,  200 },   // TR
    { rx - 8,  ry + rh, 232 },   // BL
    { rx + rw, ry + rh, 248 },   // BR
  };
  constexpr int kV = 136, kSize = 8;

  { static long np = 0; if ((np++ & 127) == 0)
      cfg_logf("panelq", "corner #%ld rect=(%d,%d %dx%d) style=%04X shadow=%u attr=%04X",
               np, rx, ry, rw, rh, style, shadow, attr); }

  for (const Corner& k : corners) {
    int xs[4] = { k.x + ox, k.x + kSize + ox, k.x + ox,          k.x + kSize + ox };
    int ys[4] = { k.y + oy, k.y + oy,         k.y + kSize + oy,  k.y + kSize + oy };
    int us[4] = { k.u, k.u + kSize, k.u,      k.u + kSize };
    int vs[4] = { kV, kV, kV + kSize, kV + kSize };
    pushQuad(c, xs, ys, us, vs, pa, tp);
  }
}

// pushDialogGlyphs — Spec 3, FUN_8007CC00 (gen shard_4.c:11855): the dialog-box per-glyph text
// emitter. Walks the flat glyph list @0x800ECB88 (count = s16 @0x1F80017E; 8B entries x@0 u16,
// y@2 u8, char@3 u8 [0x80 = double-width], u@4, v@6) emitting op-0x65 sprites, tpage 0x1F, h=16,
// w = char&0x80 ? 16 : 8. Palette follows the glyph: clut = ((char&0x7F)+0x1F0)<<6 | 0x3F — EXCEPT
// highlight mode (box+0x47==1 && box+3==1, the selected menu option): clut pinned to 0x7CBE for the
// whole row. This tap replaces the flat-list producer Render::dialogTextNative (which could not see
// the box pointer, so the highlight path was deferred — now it isn't).
void Panel::pushDialogGlyphs(Core* c, uint32_t box) {
  const int count = (int16_t)c->mem_r16(0x1F80017Eu);
  if (count <= 0 || count > 256) return;
  const bool highlight = c->mem_r8(box + 71u) == 1u && c->mem_r8(box + 3u) == 1u;
  const int ox = c->game->gpu.s_off_x, oy = c->game->gpu.s_off_y;
  { static long np = 0; if ((np++ & 127) == 0)
      cfg_logf("panelq", "dialog glyphs box=%08X count=%d hl=%d", box, count, (int)highlight); }
  for (int i = 0; i < count; i++) {
    const uint32_t e = 0x800ECB88u + (uint32_t)i * 8u;
    const int gx = (int16_t)c->mem_r16(e + 0u);
    const int gy = c->mem_r8(e + 2u);
    const uint8_t ch = c->mem_r8(e + 3u);
    const int gu = c->mem_r8(e + 4u);
    const int gv = c->mem_r8(e + 6u);
    const int gw = (ch & 0x80) ? 16 : 8, gh = 16;
    const uint32_t clut = highlight ? 0x7CBEu : ((((uint32_t)(ch & 0x7F) + 0x1F0u) << 6) | 0x3Fu);
    int xs[4] = { gx + ox, gx + gw + ox, gx + ox, gx + gw + ox };
    int ys[4] = { gy + oy, gy + oy, gy + gh + oy, gy + gh + oy };
    int us[4] = { gu, gu + gw, gu, gu + gw };
    int vs[4] = { gv, gv, gv + gh, gv + gh };
    unsigned char cc[4] = { 0x80, 0x80, 0x80, 0x80 };
    c->game->activeRq().push2dQuad(RQ_HUD, /*order_2d_fg=*/1, xs, ys, us, vs, cc, cc, cc,
                                   960, 256, 0, /*raw=*/1,
                                   (int)(clut & 0x3F) * 16, (int)(clut >> 6) & 0x1FF,
                                   0, 0, 0, 0, 0, 0, 1023, 511);
  }
}

// ---- Panel taps: own FUN_8004FFB4 (panelFill), FUN_8005019C (panelBuild) and FUN_8007CC00
// (dialog glyph row) globally — see panel.h for the architecture note (mirrors
// ScreenFade::installLeafTap).
extern void gen_func_8004FFB4(Core*);
extern void gen_func_8005019C(Core*);
extern void gen_func_8007CC00(Core*);
extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
namespace {
void panelFillTap(Core* c) {
  const uint32_t rectPtr  = c->r[4];
  const int32_t  uvIndex  = (int32_t)c->r[5];
  const uint16_t attr     = (uint16_t)c->r[6];
  const int32_t  otBucket = (int32_t)c->r[7];
  gen_func_8004FFB4(c);   // byte-exact guest packet pool / OT / stack
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // read-only overlay gate
  Panel::pushFill(c, rectPtr, uvIndex, attr, otBucket);
}
void panelBuildTap(Core* c) {
  const uint32_t rectPtr  = c->r[4];
  const uint16_t style    = (uint16_t)c->r[5];
  const uint32_t shadow   = c->r[6];
  const int32_t  otBucket = (int32_t)c->r[7];
  gen_func_8005019C(c);   // byte-exact guest packet pool / OT / stack; nests through the panelFill
                           // tap above (calls the func_8004FFB4 WRAPPER, not gen_ direct — see panel.h)
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // read-only overlay gate
  Panel::pushCorners(c, rectPtr, style, shadow, otBucket);
}
} // namespace

namespace {
void dialogGlyphsTap(Core* c) {
  const uint32_t box = c->r[4];
  gen_func_8007CC00(c);   // byte-exact guest packet pool / OT / stack
  if (c->game->oracle || c->rsub.mode.psxRender()) return;   // read-only overlay gate
  Panel::pushDialogGlyphs(c, box);
}
} // namespace

void Panel::install() {
  static bool done = false;
  if (done) return;
  done = true;
  engine_set_override_main(0x8004FFB4u, panelFillTap, gen_func_8004FFB4);
  engine_set_override_main(0x8005019Cu, panelBuildTap, gen_func_8005019C);
  engine_set_override_main(0x8007CC00u, dialogGlyphsTap, gen_func_8007CC00);
}
